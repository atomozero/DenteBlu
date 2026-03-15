/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * ObexClient — OBEX protocol client over RFCOMM or L2CAP.
 * Private to libbluetooth.so; not installed as a public header.
 */
#ifndef _OBEX_CLIENT_H_
#define _OBEX_CLIENT_H_

#include <SupportDefs.h>

#include <bluetooth/obex.h>


class RfcommSession;


class ObexClient {
public:
	/* RFCOMM transport */
							ObexClient(RfcommSession* session,
								uint8 dlci);

	/* L2CAP direct transport (GOEP 2.0).
	 * Takes ownership of the socket; closes it on destruction. */
							ObexClient(int l2capSocket);

							~ObexClient();

	/* OBEX Connect with Target UUID (16 bytes).
	 * Returns B_OK on success, sets fConnectionId. */
	status_t				Connect(const uint8* targetUuid,
								size_t uuidLen,
								const uint8* appParams = NULL,
								size_t appParamsLen = 0);

	/* OBEX GET — pull an object by name and/or type.
	 * name: Unicode object name (UTF-8, will be converted)
	 * type: ASCII content type (null-terminated)
	 * appParams: optional application parameters blob
	 * appParamsLen: length of appParams
	 * outData: receives the object body (caller must free())
	 * outLen: receives the body length */
	status_t				Get(const char* name, const char* type,
								const uint8* appParams,
								size_t appParamsLen,
								uint8** outData, size_t* outLen);

	/* OBEX PUT — push an object to the remote device.
	 * name: Unicode object name (UTF-8)
	 * type: ASCII content type (null-terminated), or NULL
	 * data: object body
	 * dataLen: body length
	 * Uses multi-packet PUT if data exceeds remote max packet size. */
	status_t				Put(const char* name, const char* type,
								const uint8* data, size_t dataLen);

	/* OBEX SETPATH — navigate to a folder.
	 * name: folder name (one level), or NULL to go up one level.
	 * flags: 0x02 = don't create. */
	status_t				SetPath(const char* name, uint8 flags = 0x02);

	/* OBEX Disconnect */
	void					Disconnect();

	bool					IsConnected() const
								{ return fConnected; }
	uint16					MaxPacketLength() const
								{ return fRemoteMaxPacket; }

private:
	/* Send a complete OBEX packet and receive the response.
	 * Response is stored in fRecvBuf[0..fRecvLen-1]. */
	status_t				_SendAndReceive(const uint8* packet,
								size_t packetLen);

	/* Read exactly 'needed' bytes into fRecvBuf
	 * starting at offset. */
	status_t				_ReadExact(size_t offset, size_t needed);

	/* Transport-level send/receive */
	ssize_t					_TransportSend(const uint8* data,
								size_t length);
	ssize_t					_TransportReceive(uint8* buffer,
								size_t maxLength, bigtime_t timeout);

	/* Header building helpers — append to fSendBuf at fSendLen */
	void					_AppendHeader4Byte(uint8 headerId,
								uint32 value);
	void					_AppendHeaderBytes(uint8 headerId,
								const uint8* data, size_t dataLen);
	void					_AppendHeaderUnicode(uint8 headerId,
								const char* utf8);

	/* Transport: RFCOMM (fSession != NULL) or L2CAP (fL2capSocket >= 0) */
	RfcommSession*			fSession;
	uint8					fDlci;
	int						fL2capSocket;

	bool					fConnected;
	uint32					fConnectionId;
	uint16					fRemoteMaxPacket;

	/* Packet buffers */
	uint8					fSendBuf[OBEX_DEFAULT_MAX_PACKET];
	size_t					fSendLen;
	uint8					fRecvBuf[OBEX_DEFAULT_MAX_PACKET];
	size_t					fRecvLen;

	/* L2CAP PDU buffer: L2CAP is message-oriented, so recv()
	 * returns a complete PDU.  We buffer any bytes that the
	 * caller didn't consume yet. */
	uint8					fL2capPduBuf[OBEX_DEFAULT_MAX_PACKET];
	size_t					fL2capPduPos;
	size_t					fL2capPduLen;
};


#endif /* _OBEX_CLIENT_H_ */
