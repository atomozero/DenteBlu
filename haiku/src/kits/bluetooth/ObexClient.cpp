/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * ObexClient — OBEX protocol client over RFCOMM or L2CAP.
 */

#include "ObexClient.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/time.h>

#include "RfcommSession.h"


#define TRACE_OBEX(fmt, ...) \
	fprintf(stderr, "OBEX: " fmt, ##__VA_ARGS__)


/* Big-endian helpers */
static inline uint16
ReadBE16(const uint8* p)
{
	return (uint16)(p[0] << 8 | p[1]);
}


static inline void
WriteBE16(uint8* p, uint16 v)
{
	p[0] = (uint8)(v >> 8);
	p[1] = (uint8)(v);
}


static inline uint32
ReadBE32(const uint8* p)
{
	return (uint32)(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]);
}


static inline void
WriteBE32(uint8* p, uint32 v)
{
	p[0] = (uint8)(v >> 24);
	p[1] = (uint8)(v >> 16);
	p[2] = (uint8)(v >> 8);
	p[3] = (uint8)(v);
}


/* =========================================================================
 * Constructor / Destructor
 * ========================================================================= */

ObexClient::ObexClient(RfcommSession* session, uint8 dlci)
	:
	fSession(session),
	fDlci(dlci),
	fL2capSocket(-1),
	fConnected(false),
	fConnectionId(0),
	fRemoteMaxPacket(255),
	fSendLen(0),
	fRecvLen(0),
	fL2capPduPos(0),
	fL2capPduLen(0)
{
}


ObexClient::ObexClient(int l2capSocket)
	:
	fSession(NULL),
	fDlci(0),
	fL2capSocket(l2capSocket),
	fConnected(false),
	fConnectionId(0),
	fRemoteMaxPacket(255),
	fSendLen(0),
	fRecvLen(0),
	fL2capPduPos(0),
	fL2capPduLen(0)
{
	/* Set 60-second receive timeout */
	struct timeval tv;
	tv.tv_sec = 60;
	tv.tv_usec = 0;
	setsockopt(fL2capSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}


ObexClient::~ObexClient()
{
	if (fConnected)
		Disconnect();

	if (fL2capSocket >= 0) {
		close(fL2capSocket);
		fL2capSocket = -1;
	}
}


/* =========================================================================
 * Transport abstraction
 * ========================================================================= */

ssize_t
ObexClient::_TransportSend(const uint8* data, size_t length)
{
	if (fSession != NULL)
		return fSession->Send(fDlci, data, length);

	if (fL2capSocket >= 0) {
		ssize_t sent = send(fL2capSocket, data, length, 0);
		if (sent < 0)
			return B_IO_ERROR;
		return sent;
	}

	return B_ERROR;
}


ssize_t
ObexClient::_TransportReceive(uint8* buffer, size_t maxLength,
	bigtime_t timeout)
{
	if (fSession != NULL)
		return fSession->Receive(fDlci, buffer, maxLength, timeout);

	if (fL2capSocket >= 0) {
		/* L2CAP is message-oriented: each recv() returns a complete
		 * PDU, so we must buffer the full PDU and serve partial reads
		 * from the buffer.  Without this, the second _ReadExact call
		 * (for the OBEX body) would find the data already discarded. */
		if (fL2capPduPos < fL2capPduLen) {
			/* Serve from buffered PDU */
			size_t avail = fL2capPduLen - fL2capPduPos;
			size_t copyLen = (maxLength < avail) ? maxLength : avail;
			memcpy(buffer, fL2capPduBuf + fL2capPduPos, copyLen);
			fL2capPduPos += copyLen;
			return (ssize_t)copyLen;
		}

		/* Buffer empty — read next complete PDU */
		struct timeval tv;
		tv.tv_sec = timeout / 1000000LL;
		tv.tv_usec = timeout % 1000000LL;
		setsockopt(fL2capSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		ssize_t got = recv(fL2capSocket, fL2capPduBuf,
			sizeof(fL2capPduBuf), 0);
		if (got < 0) {
			if (errno == ETIMEDOUT || errno == EAGAIN)
				return B_TIMED_OUT;
			return B_IO_ERROR;
		}
		if (got == 0)
			return 0;

		fL2capPduLen = (size_t)got;
		size_t copyLen = (maxLength < fL2capPduLen) ? maxLength
			: fL2capPduLen;
		memcpy(buffer, fL2capPduBuf, copyLen);
		fL2capPduPos = copyLen;
		return (ssize_t)copyLen;
	}

	return B_ERROR;
}


/* =========================================================================
 * OBEX Connect
 *
 * Request:  [0x80][Length(2)][Version=0x10][Flags=0x00][MaxPkt(2)][Headers]
 * Response: [Code(1)][Length(2)][Version(1)][Flags(1)][MaxPkt(2)][Headers]
 * ========================================================================= */

status_t
ObexClient::Connect(const uint8* targetUuid, size_t uuidLen,
	const uint8* appParams, size_t appParamsLen)
{
	if (fConnected)
		return B_BUSY;

	/* Build OBEX CONNECT request */
	fSendLen = 0;

	/* Opcode */
	fSendBuf[fSendLen++] = OBEX_OP_CONNECT;

	/* Packet length placeholder (will fill in) */
	fSendLen += 2;

	/* OBEX version 1.0 */
	fSendBuf[fSendLen++] = OBEX_VERSION;

	/* Flags = 0 */
	fSendBuf[fSendLen++] = 0x00;

	/* Our max packet length.
	 * GOEP 2.0 (L2CAP transport): shall not exceed the L2CAP MTU (1024).
	 * GOEP 1.x (RFCOMM transport): use full default (4096). */
	uint16 maxPacket = (fL2capSocket >= 0) ? 1024 : OBEX_DEFAULT_MAX_PACKET;
	WriteBE16(fSendBuf + fSendLen, maxPacket);
	fSendLen += 2;

	/* Target header (required for PBAP) */
	if (targetUuid != NULL && uuidLen > 0)
		_AppendHeaderBytes(OBEX_HDR_TARGET, targetUuid, uuidLen);

	/* Application Parameters header (e.g. PBAP SupportedFeatures) */
	if (appParams != NULL && appParamsLen > 0)
		_AppendHeaderBytes(OBEX_HDR_APP_PARAMS, appParams, appParamsLen);

	/* Fill in packet length */
	WriteBE16(fSendBuf + 1, (uint16)fSendLen);

	TRACE_OBEX("TX CONNECT (%zu bytes):", fSendLen);
	for (size_t i = 0; i < fSendLen && i < 40; i++)
		fprintf(stderr, " %02X", fSendBuf[i]);
	fprintf(stderr, "\n");

	status_t result = _SendAndReceive(fSendBuf, fSendLen);
	if (result != B_OK)
		return result;

	/* Parse response */
	if (fRecvLen < OBEX_CONNECT_HEADER_SIZE) {
		TRACE_OBEX("CONNECT response too short (%zu bytes)\n", fRecvLen);
		return B_ERROR;
	}

	uint8 responseCode = fRecvBuf[0];
	if (responseCode != OBEX_RSP_SUCCESS) {
		TRACE_OBEX("CONNECT rejected: 0x%02X\n", responseCode);
		return B_NOT_ALLOWED;
	}

	/* Remote's max packet length */
	fRemoteMaxPacket = ReadBE16(fRecvBuf + 5);
	if (fRemoteMaxPacket < 255)
		fRemoteMaxPacket = 255;

	/* For RFCOMM transport, cap the OBEX packet size so each OBEX
	 * packet fits in a single RFCOMM frame.  Some phones do not
	 * reassemble OBEX packets split across multiple RFCOMM frames,
	 * and this also avoids exhausting RFCOMM CBFC credits. */
	if (fSession != NULL) {
		uint16 rfcommLimit = fSession->Mtu();
		if (rfcommLimit < 127)
			rfcommLimit = 127;
		if (fRemoteMaxPacket > rfcommLimit)
			fRemoteMaxPacket = rfcommLimit;
	}

	TRACE_OBEX("CONNECT success, remote max packet = %u\n", fRemoteMaxPacket);

	/* Parse response headers — look for ConnectionID and Who */
	size_t off = OBEX_CONNECT_HEADER_SIZE;
	while (off < fRecvLen) {
		uint8 hi = fRecvBuf[off];
		uint8 encoding = OBEX_HDR_ENCODING(hi);

		if (encoding == OBEX_HDR_ENC_4BYTE) {
			/* 4-byte value header: HI(1) + Value(4) = 5 bytes */
			if (off + 5 > fRecvLen)
				break;
			if (hi == OBEX_HDR_CONNECTION_ID) {
				fConnectionId = ReadBE32(fRecvBuf + off + 1);
				TRACE_OBEX("  ConnectionID = 0x%08lX\n",
					(unsigned long)fConnectionId);
			}
			off += 5;
		} else if (encoding == OBEX_HDR_ENC_1BYTE) {
			/* 1-byte value header: HI(1) + Value(1) = 2 bytes */
			if (off + 2 > fRecvLen)
				break;
			if (hi == OBEX_HDR_SRM) {
				TRACE_OBEX("  SRM = 0x%02X\n", fRecvBuf[off + 1]);
			}
			off += 2;
		} else {
			/* Length-prefixed header: HI(1) + Length(2) + Data */
			if (off + 3 > fRecvLen)
				break;
			uint16 hdrLen = ReadBE16(fRecvBuf + off + 1);
			if (hdrLen < 3 || off + hdrLen > fRecvLen)
				break;
			if (hi == OBEX_HDR_WHO) {
				TRACE_OBEX("  Who header (%u bytes)\n", hdrLen - 3);
			} else if (hi == OBEX_HDR_APP_PARAMS) {
				uint16 apLen = hdrLen - 3;
				const uint8* ap = fRecvBuf + off + 3;
				TRACE_OBEX("  AppParams (%u bytes):", apLen);
				for (uint16 ai = 0; ai < apLen && ai < 32; ai++)
					fprintf(stderr, " %02X", ap[ai]);
				fprintf(stderr, "\n");
				/* Parse TLV tags */
				uint16 apOff = 0;
				while (apOff + 2 <= apLen) {
					uint8 tag = ap[apOff];
					uint8 tLen = ap[apOff + 1];
					if (apOff + 2 + tLen > apLen)
						break;
					if (tag == 0x10 && tLen == 4) {
						uint32 feat = ReadBE32(ap + apOff + 2);
						TRACE_OBEX("  PSE SupportedFeatures = 0x%08lX\n",
							(unsigned long)feat);
					} else if (tag == 0x08 && tLen == 2) {
						uint16 pbSize = ReadBE16(ap + apOff + 2);
						TRACE_OBEX("  PhonebookSize = %u\n", pbSize);
					}
					apOff += 2 + tLen;
				}
			}
			off += hdrLen;
		}
	}

	fConnected = true;
	return B_OK;
}


/* =========================================================================
 * OBEX GET
 *
 * Multi-packet: send GET requests, receive CONTINUE responses until SUCCESS.
 * First GET: includes Name/Type/AppParams + ConnectionID
 * Subsequent GETs (after CONTINUE): just opcode + ConnectionID
 * ========================================================================= */

status_t
ObexClient::Get(const char* name, const char* type,
	const uint8* appParams, size_t appParamsLen,
	uint8** outData, size_t* outLen)
{
	if (!fConnected)
		return B_NOT_ALLOWED;

	*outData = NULL;
	*outLen = 0;

	/* Accumulate body data across multiple packets */
	uint8* body = NULL;
	size_t bodyLen = 0;
	size_t bodyCapacity = 0;

	bool firstPacket = true;
	bool srmActive = false;
	bool done = false;

	while (!done) {
		/* Build GET request */
		fSendLen = 0;
		fSendBuf[fSendLen++] = OBEX_OP_GET_FINAL;
		fSendLen += 2;  /* packet length placeholder */

		/* ConnectionID header (always) */
		_AppendHeader4Byte(OBEX_HDR_CONNECTION_ID, fConnectionId);

		if (firstPacket) {
			/* SRM header — enable Single Response Mode for GOEP 2.0
			 * (OBEX over L2CAP). Only sent on first GET packet. */
			if (fL2capSocket >= 0 && fSendLen + 2 <= sizeof(fSendBuf)) {
				fSendBuf[fSendLen++] = OBEX_HDR_SRM;
				fSendBuf[fSendLen++] = 0x01;  /* Enable */
			}

			/* Name header */
			if (name != NULL && name[0] != '\0')
				_AppendHeaderUnicode(OBEX_HDR_NAME, name);

			/* Type header */
			if (type != NULL && type[0] != '\0') {
				size_t typeLen = strlen(type) + 1;  /* include null terminator */
				_AppendHeaderBytes(OBEX_HDR_TYPE, (const uint8*)type, typeLen);
			}

			/* Application Parameters header */
			if (appParams != NULL && appParamsLen > 0)
				_AppendHeaderBytes(OBEX_HDR_APP_PARAMS, appParams, appParamsLen);

			firstPacket = false;
		}

		/* Fill in packet length */
		WriteBE16(fSendBuf + 1, (uint16)fSendLen);

		/* In SRM mode, we only send the first GET request;
		 * subsequent data arrives as unsolicited responses. */
		if (!srmActive) {
			TRACE_OBEX("TX GET (%zu bytes):", fSendLen);
			for (size_t gi = 0; gi < fSendLen && gi < 80; gi++)
				fprintf(stderr, " %02X", fSendBuf[gi]);
			fprintf(stderr, "\n");

			status_t result = _SendAndReceive(fSendBuf, fSendLen);
			if (result != B_OK) {
				free(body);
				return result;
			}
		} else {
			/* SRM: just receive next response without sending */
			TRACE_OBEX("SRM: waiting for next response...\n");
			fRecvLen = 0;
			status_t result = _ReadExact(0, 3);
			if (result != B_OK) {
				free(body);
				return result;
			}

			uint16 responseLen = ReadBE16(fRecvBuf + 1);
			if (responseLen < 3 || responseLen > sizeof(fRecvBuf)) {
				TRACE_OBEX("SRM: invalid response length: %u\n",
					responseLen);
				free(body);
				return B_ERROR;
			}

			if (responseLen > 3) {
				result = _ReadExact(3, responseLen - 3);
				if (result != B_OK) {
					free(body);
					return result;
				}
			}

			fRecvLen = responseLen;

			TRACE_OBEX("SRM RX: code=0x%02X, len=%u\n",
				fRecvBuf[0], responseLen);
		}

		if (fRecvLen < 3) {
			TRACE_OBEX("GET response too short\n");
			free(body);
			return B_ERROR;
		}

		uint8 responseCode = fRecvBuf[0];

		/* Parse response headers — extract Body/EndOfBody, SRM, AppParams */
		size_t off = 3;
		while (off < fRecvLen) {
			uint8 hi = fRecvBuf[off];
			uint8 encoding = OBEX_HDR_ENCODING(hi);

			if (encoding == OBEX_HDR_ENC_4BYTE) {
				if (off + 5 > fRecvLen)
					break;
				off += 5;
			} else if (encoding == OBEX_HDR_ENC_1BYTE) {
				if (off + 2 > fRecvLen)
					break;
				if (hi == OBEX_HDR_SRM && fRecvBuf[off + 1] == 0x01) {
					TRACE_OBEX("SRM enabled by remote\n");
					srmActive = true;
				}
				off += 2;
			} else {
				/* Length-prefixed header */
				if (off + 3 > fRecvLen)
					break;
				uint16 hdrLen = ReadBE16(fRecvBuf + off + 1);
				if (hdrLen < 3 || off + hdrLen > fRecvLen)
					break;

				if (hi == OBEX_HDR_BODY || hi == OBEX_HDR_END_OF_BODY) {
					uint16 dataLen = hdrLen - 3;
					const uint8* data = fRecvBuf + off + 3;

					/* Grow body buffer if needed */
					if (bodyLen + dataLen > bodyCapacity) {
						size_t newCap = (bodyCapacity == 0) ? 4096
							: bodyCapacity * 2;
						while (newCap < bodyLen + dataLen)
							newCap *= 2;
						uint8* newBody = (uint8*)realloc(body, newCap);
						if (newBody == NULL) {
							free(body);
							return B_NO_MEMORY;
						}
						body = newBody;
						bodyCapacity = newCap;
					}

					memcpy(body + bodyLen, data, dataLen);
					bodyLen += dataLen;
				} else if (hi == OBEX_HDR_APP_PARAMS) {
					uint16 apLen = hdrLen - 3;
					const uint8* ap = fRecvBuf + off + 3;
					TRACE_OBEX("GET AppParams (%u bytes):", apLen);
					for (uint16 ai = 0; ai < apLen && ai < 32; ai++)
						fprintf(stderr, " %02X", ap[ai]);
					fprintf(stderr, "\n");
					/* Parse TLV tags for PhonebookSize */
					uint16 apOff = 0;
					while (apOff + 2 <= apLen) {
						uint8 tag = ap[apOff];
						uint8 tLen = ap[apOff + 1];
						if (apOff + 2 + tLen > apLen)
							break;
						if (tag == 0x08 && tLen == 2) {
							uint16 pbSize = ReadBE16(ap + apOff + 2);
							TRACE_OBEX("  PhonebookSize = %u\n", pbSize);
						}
						apOff += 2 + tLen;
					}
				}

				off += hdrLen;
			}
		}

		if (responseCode == OBEX_RSP_SUCCESS) {
			TRACE_OBEX("GET complete, total body = %zu bytes\n", bodyLen);
			done = true;
		} else if (responseCode == OBEX_RSP_CONTINUE) {
			TRACE_OBEX("GET continue, body so far = %zu bytes\n", bodyLen);
		} else {
			TRACE_OBEX("GET error: 0x%02X\n", responseCode);
			free(body);
			return B_ERROR;
		}
	}

	*outData = body;
	*outLen = bodyLen;
	return B_OK;
}


/* =========================================================================
 * OBEX PUT — push an object
 *
 * Multi-packet PUT:
 *   First packet:  PUT (0x02) + ConnectionID + Name + Type + Length + Body
 *   Middle packets: PUT (0x02) + Body
 *   Last packet:   PUT_FINAL (0x82) + EndOfBody
 *
 * Single-packet PUT:
 *   PUT_FINAL (0x82) + ConnectionID + Name + Type + Length + EndOfBody
 * ========================================================================= */

status_t
ObexClient::Put(const char* name, const char* type,
	const uint8* data, size_t dataLen)
{
	if (!fConnected)
		return B_NOT_ALLOWED;

	/* Calculate overhead for first packet:
	 * opcode(1) + length(2) + ConnectionID(5) + Name + Type + Length(5) */
	size_t sent = 0;
	bool firstPacket = true;

	while (sent < dataLen || firstPacket) {
		fSendLen = 0;

		bool isLast = true;  /* will be updated below */

		/* Placeholder for opcode + length */
		fSendLen++;  /* opcode — will be filled later */
		fSendLen += 2;  /* packet length placeholder */

		/* ConnectionID (always) */
		_AppendHeader4Byte(OBEX_HDR_CONNECTION_ID, fConnectionId);

		if (firstPacket) {
			/* SRM header for GOEP 2.0 */
			if (fL2capSocket >= 0 && fSendLen + 2 <= sizeof(fSendBuf)) {
				fSendBuf[fSendLen++] = OBEX_HDR_SRM;
				fSendBuf[fSendLen++] = 0x01;  /* Enable */
			}

			/* Name header */
			if (name != NULL && name[0] != '\0')
				_AppendHeaderUnicode(OBEX_HDR_NAME, name);

			/* Type header */
			if (type != NULL && type[0] != '\0') {
				size_t typeLen = strlen(type) + 1;
				_AppendHeaderBytes(OBEX_HDR_TYPE, (const uint8*)type,
					typeLen);
			}

			/* Length header (total object size) */
			_AppendHeader4Byte(OBEX_HDR_LENGTH, (uint32)dataLen);

			firstPacket = false;
		}

		/* Calculate how much body data fits in this packet.
		 * Reserve 3 bytes for body header (HI + Length) */
		size_t maxBody = fRemoteMaxPacket - fSendLen - 3;
		size_t chunkLen = dataLen - sent;
		if (chunkLen > maxBody) {
			chunkLen = maxBody;
			isLast = false;
		}

		/* Body or EndOfBody header */
		uint8 bodyHdr = isLast ? OBEX_HDR_END_OF_BODY : OBEX_HDR_BODY;
		_AppendHeaderBytes(bodyHdr, data + sent, chunkLen);
		sent += chunkLen;

		/* Set opcode */
		fSendBuf[0] = isLast ? OBEX_OP_PUT_FINAL : OBEX_OP_PUT;

		/* Fill in packet length */
		WriteBE16(fSendBuf + 1, (uint16)fSendLen);

		TRACE_OBEX("TX PUT%s (%zu bytes, body chunk %zu, "
			"total sent %zu/%zu)\n",
			isLast ? "_FINAL" : "", fSendLen, chunkLen,
			sent, dataLen);

		status_t result = _SendAndReceive(fSendBuf, fSendLen);
		if (result != B_OK)
			return result;

		uint8 responseCode = fRecvBuf[0];
		if (isLast) {
			if (responseCode != OBEX_RSP_SUCCESS) {
				TRACE_OBEX("PUT final rejected: 0x%02X\n", responseCode);
				return (responseCode == 0xC3) ? B_NOT_ALLOWED
					: (responseCode == 0xCF) ? B_BAD_TYPE
					: B_ERROR;
			}
		} else {
			if (responseCode != OBEX_RSP_CONTINUE) {
				TRACE_OBEX("PUT rejected: 0x%02X\n", responseCode);
				return (responseCode == 0xC3) ? B_NOT_ALLOWED
					: (responseCode == 0xCF) ? B_BAD_TYPE
					: B_ERROR;
			}
		}
	}

	TRACE_OBEX("PUT complete, %zu bytes sent\n", dataLen);
	return B_OK;
}


/* =========================================================================
 * OBEX SETPATH
 *
 * Request:  [0x85][Length(2)][Flags(1)][Constants(1)][Headers...]
 * Response: [Code(1)][Length(2)][Headers...]
 *
 * Flags: bit 0 = backup (go up one level)
 *        bit 1 = don't create missing folder
 * Constants: 0x00
 * ========================================================================= */

status_t
ObexClient::SetPath(const char* name, uint8 flags)
{
	if (!fConnected)
		return B_NOT_ALLOWED;

	fSendLen = 0;
	fSendBuf[fSendLen++] = OBEX_OP_SETPATH;
	fSendLen += 2;  /* length placeholder */

	/* Flags and Constants */
	fSendBuf[fSendLen++] = flags;
	fSendBuf[fSendLen++] = 0x00;

	/* ConnectionID header */
	_AppendHeader4Byte(OBEX_HDR_CONNECTION_ID, fConnectionId);

	/* Name header — empty name means go to root */
	if (name != NULL && name[0] != '\0')
		_AppendHeaderUnicode(OBEX_HDR_NAME, name);
	else {
		/* Empty Name header = go to root */
		fSendBuf[fSendLen++] = OBEX_HDR_NAME;
		WriteBE16(fSendBuf + fSendLen, 3);
		fSendLen += 2;
	}

	WriteBE16(fSendBuf + 1, (uint16)fSendLen);

	TRACE_OBEX("TX SETPATH (%zu bytes, flags=0x%02X, name=%s):",
		fSendLen, flags, name ? name : "(root)");
	for (size_t i = 0; i < fSendLen && i < 40; i++)
		fprintf(stderr, " %02X", fSendBuf[i]);
	fprintf(stderr, "\n");

	status_t result = _SendAndReceive(fSendBuf, fSendLen);
	if (result != B_OK)
		return result;

	if (fRecvLen < 3) {
		TRACE_OBEX("SETPATH response too short\n");
		return B_ERROR;
	}

	uint8 responseCode = fRecvBuf[0];
	if (responseCode != OBEX_RSP_SUCCESS) {
		TRACE_OBEX("SETPATH rejected: 0x%02X\n", responseCode);
		return B_NOT_ALLOWED;
	}

	TRACE_OBEX("SETPATH success\n");
	return B_OK;
}


/* =========================================================================
 * OBEX Disconnect
 *
 * Request:  [0x81][Length=3+headers][ConnectionID]
 * Response: [Code][Length][...]
 * ========================================================================= */

void
ObexClient::Disconnect()
{
	if (!fConnected)
		return;

	fSendLen = 0;
	fSendBuf[fSendLen++] = OBEX_OP_DISCONNECT;
	fSendLen += 2;  /* length placeholder */

	_AppendHeader4Byte(OBEX_HDR_CONNECTION_ID, fConnectionId);

	WriteBE16(fSendBuf + 1, (uint16)fSendLen);

	TRACE_OBEX("TX DISCONNECT (%zu bytes)\n", fSendLen);

	/* Fire-and-forget: send the disconnect packet but don't wait
	 * for a response.  The RFCOMM/L2CAP connection will be torn
	 * down right after, and many phones close the transport before
	 * responding — waiting would just cause a 60-second hang. */
	_TransportSend(fSendBuf, fSendLen);

	fConnected = false;
}


/* =========================================================================
 * Packet I/O
 * ========================================================================= */

status_t
ObexClient::_SendAndReceive(const uint8* packet, size_t packetLen)
{
	/* Send */
	ssize_t sent = _TransportSend(packet, packetLen);
	if (sent < 0) {
		TRACE_OBEX("Send failed: %s\n", strerror(sent));
		return (status_t)sent;
	}

	/* Receive response header (3 bytes: opcode + length) */
	TRACE_OBEX("Waiting for response (timeout=60s)...\n");
	fRecvLen = 0;
	status_t result = _ReadExact(0, 3);
	if (result != B_OK)
		return result;

	uint16 responseLen = ReadBE16(fRecvBuf + 1);
	if (responseLen < 3) {
		TRACE_OBEX("Invalid response length: %u\n", responseLen);
		return B_ERROR;
	}

	if (responseLen > sizeof(fRecvBuf)) {
		TRACE_OBEX("Response too large: %u (max %zu)\n",
			responseLen, sizeof(fRecvBuf));
		return B_ERROR;
	}

	/* Read remaining response body */
	if (responseLen > 3) {
		result = _ReadExact(3, responseLen - 3);
		if (result != B_OK)
			return result;
	}

	fRecvLen = responseLen;

	TRACE_OBEX("RX response: code=0x%02X, len=%u:", fRecvBuf[0], responseLen);
	for (size_t ri = 0; ri < fRecvLen && ri < 80; ri++)
		fprintf(stderr, " %02X", fRecvBuf[ri]);
	fprintf(stderr, "\n");

	return B_OK;
}


status_t
ObexClient::_ReadExact(size_t offset, size_t needed)
{
	size_t received = 0;

	while (received < needed) {
		ssize_t got = _TransportReceive(
			fRecvBuf + offset + received,
			needed - received, 60000000LL);

		if (got < 0) {
			TRACE_OBEX("Receive failed at offset %zu: %s\n",
				offset + received, strerror(got));
			return (status_t)got;
		}
		if (got == 0) {
			TRACE_OBEX("Receive returned 0 bytes (connection closed?)\n");
			return B_IO_ERROR;
		}
		received += (size_t)got;
	}

	return B_OK;
}


/* =========================================================================
 * Header building helpers
 * ========================================================================= */

void
ObexClient::_AppendHeader4Byte(uint8 headerId, uint32 value)
{
	if (fSendLen + 5 > sizeof(fSendBuf))
		return;

	fSendBuf[fSendLen++] = headerId;
	WriteBE32(fSendBuf + fSendLen, value);
	fSendLen += 4;
}


void
ObexClient::_AppendHeaderBytes(uint8 headerId, const uint8* data,
	size_t dataLen)
{
	uint16 hdrLen = (uint16)(3 + dataLen);
	if (fSendLen + hdrLen > sizeof(fSendBuf))
		return;

	fSendBuf[fSendLen++] = headerId;
	WriteBE16(fSendBuf + fSendLen, hdrLen);
	fSendLen += 2;
	memcpy(fSendBuf + fSendLen, data, dataLen);
	fSendLen += dataLen;
}


void
ObexClient::_AppendHeaderUnicode(uint8 headerId, const char* utf8)
{
	/* Convert UTF-8 to UCS-2 big-endian, null-terminated.
	 * Simple conversion: handles ASCII and 2-byte UTF-8 sequences.
	 * For PBAP path names, ASCII is sufficient. */
	size_t utf8Len = strlen(utf8);

	/* Worst case: each UTF-8 byte becomes 2 UCS-2 bytes, plus 2-byte null */
	size_t maxUcs2Len = (utf8Len + 1) * 2;
	uint16 hdrLen = (uint16)(3 + maxUcs2Len);
	if (fSendLen + hdrLen > sizeof(fSendBuf))
		return;

	fSendBuf[fSendLen++] = headerId;
	size_t lenPos = fSendLen;
	fSendLen += 2;  /* length placeholder */

	size_t i = 0;
	while (i < utf8Len) {
		uint16 ucs2;
		uint8 c = (uint8)utf8[i];

		if (c < 0x80) {
			ucs2 = c;
			i++;
		} else if ((c & 0xE0) == 0xC0 && i + 1 < utf8Len) {
			ucs2 = ((c & 0x1F) << 6) | (utf8[i + 1] & 0x3F);
			i += 2;
		} else if ((c & 0xF0) == 0xE0 && i + 2 < utf8Len) {
			ucs2 = ((c & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6)
				| (utf8[i + 2] & 0x3F);
			i += 3;
		} else {
			ucs2 = '?';
			i++;
		}

		WriteBE16(fSendBuf + fSendLen, ucs2);
		fSendLen += 2;
	}

	/* Null terminator (UCS-2) */
	WriteBE16(fSendBuf + fSendLen, 0x0000);
	fSendLen += 2;

	/* Fill in actual header length */
	WriteBE16(fSendBuf + lenPos, (uint16)(fSendLen - lenPos + 1));
}
