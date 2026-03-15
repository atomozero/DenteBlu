/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "SdpServer.h"

#include <stdio.h>
#include <string.h>
#include <ByteOrder.h>

#include <bluetooth/sdp.h>
#include <l2cap.h>

#include "Debug.h"


// Data Element encoding helpers (all big-endian / network byte order)

static inline size_t
WriteDE_Uint8(uint8* buf, uint8 val)
{
	buf[0] = SDP_DE_HEADER(SDP_DE_UINT, SDP_DE_SIZE_1);
	buf[1] = val;
	return 2;
}


static inline size_t
WriteDE_Uint16(uint8* buf, uint16 val)
{
	buf[0] = SDP_DE_HEADER(SDP_DE_UINT, SDP_DE_SIZE_2);
	buf[1] = (uint8)(val >> 8);
	buf[2] = (uint8)(val & 0xFF);
	return 3;
}


static inline size_t
WriteDE_Uint32(uint8* buf, uint32 val)
{
	buf[0] = SDP_DE_HEADER(SDP_DE_UINT, SDP_DE_SIZE_4);
	buf[1] = (uint8)(val >> 24);
	buf[2] = (uint8)(val >> 16);
	buf[3] = (uint8)(val >> 8);
	buf[4] = (uint8)(val & 0xFF);
	return 5;
}


static inline size_t
WriteDE_Uuid16(uint8* buf, uint16 val)
{
	buf[0] = SDP_DE_HEADER(SDP_DE_UUID, SDP_DE_SIZE_2);
	buf[1] = (uint8)(val >> 8);
	buf[2] = (uint8)(val & 0xFF);
	return 3;
}


static inline size_t
WriteDE_String(uint8* buf, const char* str, uint8 len)
{
	buf[0] = SDP_DE_HEADER(SDP_DE_STRING, SDP_DE_SIZE_NEXT1);
	buf[1] = len;
	memcpy(buf + 2, str, len);
	return 2 + len;
}


static inline size_t
WriteDE_Bool(uint8* buf, bool val)
{
	buf[0] = SDP_DE_HEADER(SDP_DE_BOOL, SDP_DE_SIZE_1);
	buf[1] = val ? 0x01 : 0x00;
	return 2;
}


// Sequence with SIZE_NEXT1: writes header (2 bytes), returns offset where
// content starts. Caller must backpatch length at buf[1] after writing
// content. Content must be <=255 bytes.
static inline size_t
BeginDE_Sequence(uint8* buf)
{
	buf[0] = SDP_DE_HEADER(SDP_DE_SEQUENCE, SDP_DE_SIZE_NEXT1);
	buf[1] = 0;
	return 2;
}


static inline void
PatchDE_SequenceLength(uint8* seqStart, uint16 contentLen)
{
	seqStart[1] = (uint8)(contentLen & 0xFF);
}


// Write a single attribute entry: uint16 attribute ID + value
static inline size_t
WriteDE_AttrId(uint8* buf, uint16 attrId)
{
	return WriteDE_Uint16(buf, attrId);
}


// Build SDP PDU header
static inline void
WritePduHeader(uint8* buf, uint8 pduId, uint16 transactionId,
	uint16 paramLength)
{
	buf[0] = pduId;
	buf[1] = (uint8)(transactionId >> 8);
	buf[2] = (uint8)(transactionId & 0xFF);
	buf[3] = (uint8)(paramLength >> 8);
	buf[4] = (uint8)(paramLength & 0xFF);
}


static inline uint16
ReadBE16(const uint8* buf)
{
	return ((uint16)buf[0] << 8) | buf[1];
}


// SdpServer implementation

SdpServer::SdpServer()
	:
	fSdpServerRecordLen(0),
	fPnpInfoRecordLen(0),
	fSppRecordLen(0),
	fPbapPceRecordLen(0),
	fOppRecordLen(0),
	fHfpRecordLen(0),
	fA2dpSinkRecordLen(0),
	fA2dpSourceRecordLen(0),
	fAvrcpTargetRecordLen(0),
	fHfpAgRecordLen(0),
	fSdpServerUuidCount(0),
	fPnpInfoUuidCount(0),
	fSppUuidCount(0),
	fPbapPceUuidCount(0),
	fOppUuidCount(0),
	fHfpUuidCount(0),
	fA2dpSinkUuidCount(0),
	fA2dpSourceUuidCount(0),
	fAvrcpTargetUuidCount(0),
	fHfpAgUuidCount(0)
{
	memset(fSdpServerRecord, 0, sizeof(fSdpServerRecord));
	memset(fPnpInfoRecord, 0, sizeof(fPnpInfoRecord));
	memset(fSppRecord, 0, sizeof(fSppRecord));
	memset(fPbapPceRecord, 0, sizeof(fPbapPceRecord));
	memset(fOppRecord, 0, sizeof(fOppRecord));
	memset(fHfpRecord, 0, sizeof(fHfpRecord));
	memset(fA2dpSinkRecord, 0, sizeof(fA2dpSinkRecord));
	memset(fA2dpSourceRecord, 0, sizeof(fA2dpSourceRecord));
	memset(fAvrcpTargetRecord, 0, sizeof(fAvrcpTargetRecord));
	memset(fHfpAgRecord, 0, sizeof(fHfpAgRecord));
	memset(fSdpServerUuids, 0, sizeof(fSdpServerUuids));
	memset(fPnpInfoUuids, 0, sizeof(fPnpInfoUuids));
	memset(fSppUuids, 0, sizeof(fSppUuids));
	memset(fPbapPceUuids, 0, sizeof(fPbapPceUuids));
	memset(fOppUuids, 0, sizeof(fOppUuids));
	memset(fHfpUuids, 0, sizeof(fHfpUuids));
	memset(fA2dpSinkUuids, 0, sizeof(fA2dpSinkUuids));
	memset(fA2dpSourceUuids, 0, sizeof(fA2dpSourceUuids));
	memset(fAvrcpTargetUuids, 0, sizeof(fAvrcpTargetUuids));
	memset(fHfpAgUuids, 0, sizeof(fHfpAgUuids));

	_BuildServiceRecords();
}


SdpServer::~SdpServer()
{
}


ssize_t
SdpServer::HandleRequest(const uint8* request, size_t requestLen,
	uint8* response, size_t responseMaxLen)
{
	if (requestLen < SDP_PDU_HEADER_SIZE) {
		TRACE_BT("SDP: Request too short (%zu bytes)\n", requestLen);
		return -1;
	}

	uint8 pduId = request[0];
	uint16 transactionId = ReadBE16(request + 1);
	uint16 paramLength = ReadBE16(request + 3);

	if ((size_t)(SDP_PDU_HEADER_SIZE + paramLength) > requestLen) {
		TRACE_BT("SDP: PDU param length %u exceeds request size %zu\n",
			paramLength, requestLen);
		return _BuildErrorRsp(transactionId, SDP_ERR_INVALID_PDU_SIZE,
			response, responseMaxLen);
	}

	const uint8* params = request + SDP_PDU_HEADER_SIZE;

	TRACE_BT("SDP: PDU id=0x%02x txn=%u paramLen=%u reqHex:", pduId,
		transactionId, paramLength);
	for (size_t i = 0; i < requestLen && i < 32; i++)
		TRACE_BT(" %02X", request[i]);
	TRACE_BT("\n");

	switch (pduId) {
		case SDP_SERVICE_SEARCH_REQ:
			return _HandleServiceSearchReq(transactionId, params,
				paramLength, response, responseMaxLen);

		case SDP_SERVICE_ATTR_REQ:
			return _HandleServiceAttrReq(transactionId, params,
				paramLength, response, responseMaxLen);

		case SDP_SERVICE_SEARCH_ATTR_REQ:
			return _HandleServiceSearchAttrReq(transactionId, params,
				paramLength, response, responseMaxLen);

		default:
			TRACE_BT("SDP: Unknown PDU id 0x%02x\n", pduId);
			return _BuildErrorRsp(transactionId, SDP_ERR_INVALID_SYNTAX,
				response, responseMaxLen);
	}
}


ssize_t
SdpServer::_HandleServiceSearchReq(uint16 transactionId,
	const uint8* params, size_t paramLen, uint8* response,
	size_t responseMaxLen)
{
	// Parse UUID search pattern
	uint16 uuids[8];
	uint8 uuidCount = 0;
	size_t consumed = 0;

	if (!_ParseUuidList(params, paramLen, uuids, 8, uuidCount, consumed)) {
		TRACE_BT("SDP: Failed to parse UUID list in ServiceSearchReq\n");
		return _BuildErrorRsp(transactionId, SDP_ERR_INVALID_SYNTAX,
			response, responseMaxLen);
	}

	// Skip MaximumServiceRecordCount (2 bytes) + ContinuationState
	// We just need to find matching records and return their handles

	// Find matching records
	uint32 handles[10];
	uint16 totalFound = 0;

	if (_RecordMatchesUuid(0, uuids, uuidCount))
		handles[totalFound++] = 0x00000000;
	if (_RecordMatchesUuid(1, uuids, uuidCount))
		handles[totalFound++] = 0x00010001;
	if (_RecordMatchesUuid(2, uuids, uuidCount))
		handles[totalFound++] = 0x00020001;
	if (_RecordMatchesUuid(3, uuids, uuidCount))
		handles[totalFound++] = 0x00030001;
	if (_RecordMatchesUuid(4, uuids, uuidCount))
		handles[totalFound++] = 0x00040001;
	if (_RecordMatchesUuid(5, uuids, uuidCount))
		handles[totalFound++] = 0x00050001;
	if (_RecordMatchesUuid(6, uuids, uuidCount))
		handles[totalFound++] = 0x00060001;
	if (_RecordMatchesUuid(7, uuids, uuidCount))
		handles[totalFound++] = 0x00070001;
	if (_RecordMatchesUuid(8, uuids, uuidCount))
		handles[totalFound++] = 0x00080001;
	if (_RecordMatchesUuid(9, uuids, uuidCount))
		handles[totalFound++] = 0x00090001;

	// Build response: TotalServiceRecordCount(2) + CurrentServiceRecordCount(2)
	// + ServiceRecordHandleList(4*N) + ContinuationState(1=0)
	uint16 respParamLen = 2 + 2 + (4 * totalFound) + 1;
	if ((size_t)(SDP_PDU_HEADER_SIZE + respParamLen) > responseMaxLen)
		return _BuildErrorRsp(transactionId, SDP_ERR_INSUFFICIENT_RESOURCES,
			response, responseMaxLen);

	WritePduHeader(response, SDP_SERVICE_SEARCH_RSP, transactionId,
		respParamLen);

	uint8* p = response + SDP_PDU_HEADER_SIZE;
	// TotalServiceRecordCount
	p[0] = (uint8)(totalFound >> 8);
	p[1] = (uint8)(totalFound & 0xFF);
	p += 2;
	// CurrentServiceRecordCount
	p[0] = (uint8)(totalFound >> 8);
	p[1] = (uint8)(totalFound & 0xFF);
	p += 2;
	// ServiceRecordHandleList
	for (uint16 i = 0; i < totalFound; i++) {
		p[0] = (uint8)(handles[i] >> 24);
		p[1] = (uint8)(handles[i] >> 16);
		p[2] = (uint8)(handles[i] >> 8);
		p[3] = (uint8)(handles[i] & 0xFF);
		p += 4;
	}
	// ContinuationState = 0 (no continuation)
	p[0] = 0x00;

	TRACE_BT("SDP: ServiceSearchRsp: %u records found\n", totalFound);
	return SDP_PDU_HEADER_SIZE + respParamLen;
}


ssize_t
SdpServer::_HandleServiceAttrReq(uint16 transactionId,
	const uint8* params, size_t paramLen, uint8* response,
	size_t responseMaxLen)
{
	// Parse ServiceRecordHandle (4 bytes)
	if (paramLen < 4) {
		return _BuildErrorRsp(transactionId, SDP_ERR_INVALID_SYNTAX,
			response, responseMaxLen);
	}

	uint32 handle = ((uint32)params[0] << 24) | ((uint32)params[1] << 16)
		| ((uint32)params[2] << 8) | params[3];

	// Find the record
	const uint8* record = NULL;
	size_t recordLen = 0;

	if (handle == 0x00000000) {
		record = fSdpServerRecord;
		recordLen = fSdpServerRecordLen;
	} else if (handle == 0x00010001) {
		record = fPnpInfoRecord;
		recordLen = fPnpInfoRecordLen;
	} else if (handle == 0x00020001) {
		record = fSppRecord;
		recordLen = fSppRecordLen;
	} else if (handle == 0x00030001) {
		record = fPbapPceRecord;
		recordLen = fPbapPceRecordLen;
	} else if (handle == 0x00040001) {
		record = fOppRecord;
		recordLen = fOppRecordLen;
	} else if (handle == 0x00050001) {
		record = fHfpRecord;
		recordLen = fHfpRecordLen;
	} else if (handle == 0x00060001) {
		record = fA2dpSinkRecord;
		recordLen = fA2dpSinkRecordLen;
	} else if (handle == 0x00070001) {
		record = fA2dpSourceRecord;
		recordLen = fA2dpSourceRecordLen;
	} else if (handle == 0x00080001) {
		record = fAvrcpTargetRecord;
		recordLen = fAvrcpTargetRecordLen;
	} else if (handle == 0x00090001) {
		record = fHfpAgRecord;
		recordLen = fHfpAgRecordLen;
	}

	if (record == NULL) {
		return _BuildErrorRsp(transactionId, SDP_ERR_INVALID_RECORD_HANDLE,
			response, responseMaxLen);
	}

	// Build response: AttributeListByteCount(2) + AttributeList + ContinuationState(1)
	uint16 respParamLen = 2 + recordLen + 1;
	if ((size_t)(SDP_PDU_HEADER_SIZE + respParamLen) > responseMaxLen)
		return _BuildErrorRsp(transactionId, SDP_ERR_INSUFFICIENT_RESOURCES,
			response, responseMaxLen);

	WritePduHeader(response, SDP_SERVICE_ATTR_RSP, transactionId,
		respParamLen);

	uint8* p = response + SDP_PDU_HEADER_SIZE;
	// AttributeListByteCount
	p[0] = (uint8)(recordLen >> 8);
	p[1] = (uint8)(recordLen & 0xFF);
	p += 2;
	// AttributeList (the pre-built record is already a DE sequence)
	memcpy(p, record, recordLen);
	p += recordLen;
	// ContinuationState = 0
	p[0] = 0x00;

	TRACE_BT("SDP: ServiceAttrRsp for handle 0x%08" B_PRIx32 ": %zu bytes\n",
		handle, recordLen);
	return SDP_PDU_HEADER_SIZE + respParamLen;
}


ssize_t
SdpServer::_HandleServiceSearchAttrReq(uint16 transactionId,
	const uint8* params, size_t paramLen, uint8* response,
	size_t responseMaxLen)
{
	// 1. Parse UUID search pattern (Data Element Sequence of UUID16)
	uint16 uuids[8];
	uint8 uuidCount = 0;
	size_t consumed = 0;

	if (!_ParseUuidList(params, paramLen, uuids, 8, uuidCount, consumed)) {
		TRACE_BT("SDP: Failed to parse UUID list in ServiceSearchAttrReq\n");
		return _BuildErrorRsp(transactionId, SDP_ERR_INVALID_SYNTAX,
			response, responseMaxLen);
	}

	const uint8* p = params + consumed;
	size_t remaining = paramLen - consumed;

	// 2. Parse MaximumAttributeByteCount (uint16)
	if (remaining < 2) {
		return _BuildErrorRsp(transactionId, SDP_ERR_INVALID_SYNTAX,
			response, responseMaxLen);
	}
	// uint16 maxAttrByteCount = ReadBE16(p);  // We don't limit for now
	p += 2;
	remaining -= 2;

	// 3. Parse AttributeIDList (Data Element Sequence — we accept but ignore
	//    specific attribute filtering; we return all attributes)
	// Skip the attribute list DE sequence
	if (remaining < 1) {
		return _BuildErrorRsp(transactionId, SDP_ERR_INVALID_SYNTAX,
			response, responseMaxLen);
	}

	// Skip over the AttributeIDList DE sequence
	uint8 deHeader = p[0];
	uint8 sizeDesc = deHeader & 0x07;
	size_t attrListLen = 0;
	size_t attrListHeaderLen = 1;

	switch (sizeDesc) {
		case SDP_DE_SIZE_NEXT1:
			if (remaining < 2) goto syntax_error;
			attrListLen = p[1];
			attrListHeaderLen = 2;
			break;
		case SDP_DE_SIZE_NEXT2:
			if (remaining < 3) goto syntax_error;
			attrListLen = ReadBE16(p + 1);
			attrListHeaderLen = 3;
			break;
		case SDP_DE_SIZE_NEXT4:
			if (remaining < 5) goto syntax_error;
			attrListLen = ((uint32)p[1] << 24) | ((uint32)p[2] << 16)
				| ((uint32)p[3] << 8) | p[4];
			attrListHeaderLen = 5;
			break;
		default:
			goto syntax_error;
	}

	p += attrListHeaderLen + attrListLen;
	remaining -= attrListHeaderLen + attrListLen;

	// 4. ContinuationState — we only handle first request (no continuation)
	if (remaining < 1) {
		return _BuildErrorRsp(transactionId, SDP_ERR_INVALID_SYNTAX,
			response, responseMaxLen);
	}
	// uint8 contStateLen = p[0];  // Should be 0 for first request

	TRACE_BT("SDP: ServiceSearchAttrReq: %u UUIDs in search pattern\n",
		uuidCount);
	for (uint8 i = 0; i < uuidCount; i++)
		TRACE_BT("SDP:   UUID[%u] = 0x%04x\n", i, uuids[i]);

	{
		// 5. Build response: collect matching records into an outer
		//    Data Element Sequence
		uint8* resp = response + SDP_PDU_HEADER_SIZE;
		size_t maxRespPayload = responseMaxLen - SDP_PDU_HEADER_SIZE;

		// We need: AttributeListsByteCount(2) + outer_seq + records +
		//   ContinuationState(1)
		// Start building after the 2-byte count field
		uint8* outerSeq = resp + 2;
		size_t outerSeqHeader = BeginDE_Sequence(outerSeq);
		size_t outerContent = 0;

		// Check each record
		if (_RecordMatchesUuid(0, uuids, uuidCount)) {
			if (outerSeqHeader + outerContent + fSdpServerRecordLen
					< maxRespPayload - 3) {
				memcpy(outerSeq + outerSeqHeader + outerContent,
					fSdpServerRecord, fSdpServerRecordLen);
				outerContent += fSdpServerRecordLen;
			}
		}

		if (_RecordMatchesUuid(1, uuids, uuidCount)) {
			if (outerSeqHeader + outerContent + fPnpInfoRecordLen
					< maxRespPayload - 3) {
				memcpy(outerSeq + outerSeqHeader + outerContent,
					fPnpInfoRecord, fPnpInfoRecordLen);
				outerContent += fPnpInfoRecordLen;
			}
		}

		if (_RecordMatchesUuid(2, uuids, uuidCount)) {
			if (outerSeqHeader + outerContent + fSppRecordLen
					< maxRespPayload - 3) {
				memcpy(outerSeq + outerSeqHeader + outerContent,
					fSppRecord, fSppRecordLen);
				outerContent += fSppRecordLen;
			}
		}

		if (_RecordMatchesUuid(3, uuids, uuidCount)) {
			if (outerSeqHeader + outerContent + fPbapPceRecordLen
					< maxRespPayload - 3) {
				memcpy(outerSeq + outerSeqHeader + outerContent,
					fPbapPceRecord, fPbapPceRecordLen);
				outerContent += fPbapPceRecordLen;
			}
		}

		if (_RecordMatchesUuid(4, uuids, uuidCount)) {
			if (outerSeqHeader + outerContent + fOppRecordLen
					< maxRespPayload - 3) {
				memcpy(outerSeq + outerSeqHeader + outerContent,
					fOppRecord, fOppRecordLen);
				outerContent += fOppRecordLen;
			}
		}

		if (_RecordMatchesUuid(5, uuids, uuidCount)) {
			if (outerSeqHeader + outerContent + fHfpRecordLen
					< maxRespPayload - 3) {
				memcpy(outerSeq + outerSeqHeader + outerContent,
					fHfpRecord, fHfpRecordLen);
				outerContent += fHfpRecordLen;
			}
		}

		if (_RecordMatchesUuid(6, uuids, uuidCount)) {
			if (outerSeqHeader + outerContent + fA2dpSinkRecordLen
					< maxRespPayload - 3) {
				memcpy(outerSeq + outerSeqHeader + outerContent,
					fA2dpSinkRecord, fA2dpSinkRecordLen);
				outerContent += fA2dpSinkRecordLen;
			}
		}

		if (_RecordMatchesUuid(7, uuids, uuidCount)) {
			if (outerSeqHeader + outerContent + fA2dpSourceRecordLen
					< maxRespPayload - 3) {
				memcpy(outerSeq + outerSeqHeader + outerContent,
					fA2dpSourceRecord, fA2dpSourceRecordLen);
				outerContent += fA2dpSourceRecordLen;
			}
		}

		if (_RecordMatchesUuid(8, uuids, uuidCount)) {
			if (outerSeqHeader + outerContent + fAvrcpTargetRecordLen
					< maxRespPayload - 3) {
				memcpy(outerSeq + outerSeqHeader + outerContent,
					fAvrcpTargetRecord, fAvrcpTargetRecordLen);
				outerContent += fAvrcpTargetRecordLen;
			}
		}

		if (_RecordMatchesUuid(9, uuids, uuidCount)) {
			if (outerSeqHeader + outerContent + fHfpAgRecordLen
					< maxRespPayload - 3) {
				memcpy(outerSeq + outerSeqHeader + outerContent,
					fHfpAgRecord, fHfpAgRecordLen);
				outerContent += fHfpAgRecordLen;
			}
		}

		// Backpatch outer sequence length
		PatchDE_SequenceLength(outerSeq, outerContent);

		size_t totalAttrBytes = outerSeqHeader + outerContent;

		// AttributeListsByteCount
		resp[0] = (uint8)(totalAttrBytes >> 8);
		resp[1] = (uint8)(totalAttrBytes & 0xFF);

		// ContinuationState = 0 (no continuation)
		resp[2 + totalAttrBytes] = 0x00;

		uint16 respParamLen = 2 + totalAttrBytes + 1;

		WritePduHeader(response, SDP_SERVICE_SEARCH_ATTR_RSP,
			transactionId, respParamLen);

		TRACE_BT("SDP: ServiceSearchAttrRsp: %zu bytes payload\n",
			totalAttrBytes);

		// Hex dump the full response for debugging
		size_t totalResp = SDP_PDU_HEADER_SIZE + respParamLen;
		TRACE_BT("SDP: Response hex (%zu bytes):", totalResp);
		for (size_t i = 0; i < totalResp; i++) {
			if (i % 16 == 0)
				TRACE_BT("\nSDP:  ");
			TRACE_BT(" %02X", response[i]);
		}
		TRACE_BT("\n");

		return totalResp;
	}

syntax_error:
	return _BuildErrorRsp(transactionId, SDP_ERR_INVALID_SYNTAX,
		response, responseMaxLen);
}


ssize_t
SdpServer::_BuildErrorRsp(uint16 transactionId, uint16 errorCode,
	uint8* response, size_t responseMaxLen)
{
	if (responseMaxLen < SDP_PDU_HEADER_SIZE + 2)
		return -1;

	WritePduHeader(response, SDP_ERROR_RSP, transactionId, 2);
	response[5] = (uint8)(errorCode >> 8);
	response[6] = (uint8)(errorCode & 0xFF);

	TRACE_BT("SDP: ErrorRsp: code=0x%04x\n", errorCode);
	return SDP_PDU_HEADER_SIZE + 2;
}


bool
SdpServer::_ParseUuidFromDE(const uint8* data, size_t len,
	uint16& uuid, size_t& consumed)
{
	if (len < 1)
		return false;

	uint8 header = data[0];
	uint8 type = header >> 3;
	uint8 sizeDesc = header & 0x07;

	if (type != SDP_DE_UUID)
		return false;

	switch (sizeDesc) {
		case SDP_DE_SIZE_2:	// UUID16
			if (len < 3)
				return false;
			uuid = ReadBE16(data + 1);
			consumed = 3;
			return true;

		case SDP_DE_SIZE_4:	// UUID32 — take lower 16 bits
			if (len < 5)
				return false;
			uuid = ReadBE16(data + 3);
			consumed = 5;
			return true;

		case SDP_DE_SIZE_16:	// UUID128 — take bytes 2-3 (short UUID in
								// Bluetooth Base UUID)
			if (len < 17)
				return false;
			uuid = ReadBE16(data + 3);
			consumed = 17;
			return true;

		default:
			return false;
	}
}


bool
SdpServer::_ParseUuidList(const uint8* data, size_t len,
	uint16* uuids, uint8 maxUuids, uint8& uuidCount, size_t& consumed)
{
	if (len < 1)
		return false;

	uint8 header = data[0];
	uint8 type = header >> 3;
	uint8 sizeDesc = header & 0x07;

	if (type != SDP_DE_SEQUENCE)
		return false;

	size_t seqLen = 0;
	size_t headerLen = 1;

	switch (sizeDesc) {
		case SDP_DE_SIZE_NEXT1:
			if (len < 2) return false;
			seqLen = data[1];
			headerLen = 2;
			break;
		case SDP_DE_SIZE_NEXT2:
			if (len < 3) return false;
			seqLen = ReadBE16(data + 1);
			headerLen = 3;
			break;
		case SDP_DE_SIZE_NEXT4:
			if (len < 5) return false;
			seqLen = ((uint32)data[1] << 24) | ((uint32)data[2] << 16)
				| ((uint32)data[3] << 8) | data[4];
			headerLen = 5;
			break;
		default:
			return false;
	}

	if (headerLen + seqLen > len)
		return false;

	const uint8* seqData = data + headerLen;
	size_t seqRemaining = seqLen;
	uuidCount = 0;

	while (seqRemaining > 0 && uuidCount < maxUuids) {
		uint16 uuid;
		size_t elemConsumed;

		if (!_ParseUuidFromDE(seqData, seqRemaining, uuid, elemConsumed))
			break;

		uuids[uuidCount++] = uuid;
		seqData += elemConsumed;
		seqRemaining -= elemConsumed;
	}

	consumed = headerLen + seqLen;
	return uuidCount > 0;
}


void
SdpServer::_BuildServiceRecords()
{
	fSdpServerRecordLen = _BuildSdpServerRecord(fSdpServerRecord,
		sizeof(fSdpServerRecord));
	fPnpInfoRecordLen = _BuildPnpInfoRecord(fPnpInfoRecord,
		sizeof(fPnpInfoRecord));

	// Populate UUID lookup arrays
	fSdpServerUuids[0] = SDP_UUID16_SDP_SERVER;		// 0x1000
	fSdpServerUuids[1] = SDP_UUID16_L2CAP;				// 0x0100
	fSdpServerUuids[2] = SDP_UUID16_SDP;				// 0x0001
	fSdpServerUuids[3] = SDP_UUID16_PUBLIC_BROWSE_ROOT;	// 0x1002
	fSdpServerUuidCount = 4;

	fPnpInfoUuids[0] = SDP_UUID16_PNP_INFORMATION;		// 0x1200
	fPnpInfoUuids[1] = SDP_UUID16_PUBLIC_BROWSE_ROOT;	// 0x1002
	fPnpInfoUuidCount = 2;

	fSppRecordLen = _BuildSppRecord(fSppRecord, sizeof(fSppRecord), 1);

	fSppUuids[0] = SDP_UUID16_SERIAL_PORT;				// 0x1101
	fSppUuids[1] = SDP_UUID16_L2CAP;					// 0x0100
	fSppUuids[2] = SDP_UUID16_RFCOMM;					// 0x0003
	fSppUuids[3] = SDP_UUID16_PUBLIC_BROWSE_ROOT;		// 0x1002
	fSppUuidCount = 4;

	fPbapPceRecordLen = _BuildPbapPceRecord(fPbapPceRecord,
		sizeof(fPbapPceRecord));

	fPbapPceUuids[0] = SDP_UUID16_PBAP_PCE;			// 0x112E
	fPbapPceUuids[1] = SDP_UUID16_L2CAP;				// 0x0100
	fPbapPceUuids[2] = SDP_UUID16_RFCOMM;				// 0x0003
	fPbapPceUuids[3] = SDP_UUID16_PUBLIC_BROWSE_ROOT;	// 0x1002
	fPbapPceUuidCount = 4;

	// OPP record: RFCOMM channel 2 (channel 1 is SPP)
	fOppRecordLen = _BuildOppRecord(fOppRecord, sizeof(fOppRecord), 2);

	fOppUuids[0] = SDP_UUID16_OBEX_OBJECT_PUSH;		// 0x1105
	fOppUuids[1] = SDP_UUID16_L2CAP;					// 0x0100
	fOppUuids[2] = SDP_UUID16_RFCOMM;					// 0x0003
	fOppUuids[3] = SDP_UUID16_PUBLIC_BROWSE_ROOT;		// 0x1002
	fOppUuidCount = 4;

	// HFP HF record: RFCOMM channel 3
	fHfpRecordLen = _BuildHfpRecord(fHfpRecord, sizeof(fHfpRecord), 3);

	fHfpUuids[0] = SDP_UUID16_HFP_HF;				// 0x111E
	fHfpUuids[1] = SDP_UUID16_L2CAP;				// 0x0100
	fHfpUuids[2] = SDP_UUID16_RFCOMM;				// 0x0003
	fHfpUuids[3] = SDP_UUID16_GENERIC_AUDIO;		// 0x1203
	fHfpUuidCount = 4;

	// A2DP Sink record
	fA2dpSinkRecordLen = _BuildA2dpSinkRecord(fA2dpSinkRecord,
		sizeof(fA2dpSinkRecord));

	fA2dpSinkUuids[0] = SDP_UUID16_A2DP_SINK;		// 0x110B
	fA2dpSinkUuids[1] = SDP_UUID16_L2CAP;			// 0x0100
	fA2dpSinkUuids[2] = SDP_UUID16_AVDTP;			// 0x0019
	fA2dpSinkUuidCount = 3;

	// A2DP Source record
	fA2dpSourceRecordLen = _BuildA2dpSourceRecord(fA2dpSourceRecord,
		sizeof(fA2dpSourceRecord));

	fA2dpSourceUuids[0] = SDP_UUID16_A2DP_SOURCE;	// 0x110A
	fA2dpSourceUuids[1] = SDP_UUID16_L2CAP;		// 0x0100
	fA2dpSourceUuids[2] = SDP_UUID16_AVDTP;		// 0x0019
	fA2dpSourceUuidCount = 3;

	// AVRCP Target record
	fAvrcpTargetRecordLen = _BuildAvrcpTargetRecord(fAvrcpTargetRecord,
		sizeof(fAvrcpTargetRecord));

	fAvrcpTargetUuids[0] = SDP_UUID16_AVRCP_TARGET;	// 0x110C
	fAvrcpTargetUuids[1] = SDP_UUID16_L2CAP;			// 0x0100
	fAvrcpTargetUuids[2] = SDP_UUID16_AVCTP;			// 0x0017
	fAvrcpTargetUuids[3] = SDP_UUID16_AVRCP;			// 0x110E
	fAvrcpTargetUuidCount = 4;

	// HFP AG record: RFCOMM channel 4
	fHfpAgRecordLen = _BuildHfpAgRecord(fHfpAgRecord,
		sizeof(fHfpAgRecord), 4);

	fHfpAgUuids[0] = SDP_UUID16_HFP_AG;			// 0x111F
	fHfpAgUuids[1] = SDP_UUID16_L2CAP;				// 0x0100
	fHfpAgUuids[2] = SDP_UUID16_RFCOMM;			// 0x0003
	fHfpAgUuids[3] = SDP_UUID16_GENERIC_AUDIO;		// 0x1203
	fHfpAgUuidCount = 4;

	TRACE_BT("SDP: Built %zu bytes SDP server record, %zu bytes PnP record, "
		"%zu bytes SPP record, %zu bytes PBAP PCE record, "
		"%zu bytes OPP record, %zu bytes HFP record, "
		"%zu bytes A2DP Sink record, %zu bytes A2DP Source record, "
		"%zu bytes AVRCP Target record, %zu bytes HFP AG record\n",
		fSdpServerRecordLen, fPnpInfoRecordLen, fSppRecordLen,
		fPbapPceRecordLen, fOppRecordLen, fHfpRecordLen,
		fA2dpSinkRecordLen, fA2dpSourceRecordLen,
		fAvrcpTargetRecordLen, fHfpAgRecordLen);
}


size_t
SdpServer::_BuildSdpServerRecord(uint8* buf, size_t maxLen)
{
	// Build a DE Sequence containing attribute ID/value pairs
	// for the SDP Server service record (handle 0x00000000)

	uint8* start = buf;
	size_t headerSize = BeginDE_Sequence(buf);
	uint8* p = buf + headerSize;

	// ServiceRecordHandle (0x0000) = uint32(0x00000000)
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_RECORD_HANDLE);
	p += WriteDE_Uint32(p, 0x00000000);

	// ServiceClassIDList (0x0001) = Sequence { UUID16(0x1000) }
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_CLASS_ID_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_SDP_SERVER);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProtocolDescriptorList (0x0004) =
	//   Sequence { Sequence { UUID16(L2CAP), uint16(PSM=1) },
	//              Sequence { UUID16(SDP) } }
	p += WriteDE_AttrId(p, SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		// L2CAP with PSM
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_L2CAP);
			ip += WriteDE_Uint16(ip, SDP_UUID16_SDP);  // PSM = 0x0001
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}
		// SDP
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_SDP);
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// BrowseGroupList (0x0005) = Sequence { UUID16(0x1002) }
	p += WriteDE_AttrId(p, SDP_ATTR_BROWSE_GROUP_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_PUBLIC_BROWSE_ROOT);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// LanguageBaseAttributeIDList (0x0006) =
	//   Sequence { uint16(0x656E "en"), uint16(0x006A UTF-8),
	//              uint16(0x0100 base) }
	p += WriteDE_AttrId(p, SDP_ATTR_LANGUAGE_BASE_ATTR_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uint16(sp, 0x656E);  // "en" (ISO 639)
		sp += WriteDE_Uint16(sp, 0x006A);  // UTF-8 (IANA MIBenum 106)
		sp += WriteDE_Uint16(sp, 0x0100);  // attribute ID base
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ServiceName (0x0100) = String("Service Discovery")
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_NAME);
	p += WriteDE_String(p, "Service Discovery", 17);

	// Patch the outer sequence length
	PatchDE_SequenceLength(start, p - start - headerSize);

	return p - start;
}


size_t
SdpServer::_BuildPnpInfoRecord(uint8* buf, size_t maxLen)
{
	// Build a DE Sequence for the PnP Information service record
	// (handle 0x00010001)

	uint8* start = buf;
	size_t headerSize = BeginDE_Sequence(buf);
	uint8* p = buf + headerSize;

	// ServiceRecordHandle (0x0000) = uint32(0x00010001)
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_RECORD_HANDLE);
	p += WriteDE_Uint32(p, 0x00010001);

	// ServiceClassIDList (0x0001) = Sequence { UUID16(0x1200) }
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_CLASS_ID_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_PNP_INFORMATION);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// BrowseGroupList (0x0005) = Sequence { UUID16(0x1002) }
	p += WriteDE_AttrId(p, SDP_ATTR_BROWSE_GROUP_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_PUBLIC_BROWSE_ROOT);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// BluetoothProfileDescriptorList (0x0009) =
	//   Sequence { Sequence { UUID16(0x1200), uint16(0x0103) } }
	p += WriteDE_AttrId(p, SDP_ATTR_PROFILE_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_PNP_INFORMATION);
			ip += WriteDE_Uint16(ip, 0x0103);
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// SpecificationID (0x0200) = uint16(0x0102)
	p += WriteDE_AttrId(p, SDP_ATTR_SPECIFICATION_ID);
	p += WriteDE_Uint16(p, 0x0102);

	// VendorID (0x0201) = uint16(0x0000)
	p += WriteDE_AttrId(p, SDP_ATTR_VENDOR_ID);
	p += WriteDE_Uint16(p, 0x0000);

	// ProductID (0x0202) = uint16(0x0000)
	p += WriteDE_AttrId(p, SDP_ATTR_PRODUCT_ID);
	p += WriteDE_Uint16(p, 0x0000);

	// Version (0x0203) = uint16(0x0001)
	p += WriteDE_AttrId(p, SDP_ATTR_VERSION);
	p += WriteDE_Uint16(p, 0x0001);

	// PrimaryRecord (0x0204) = bool(true)
	p += WriteDE_AttrId(p, SDP_ATTR_PRIMARY_RECORD);
	p += WriteDE_Bool(p, true);

	// VendorIDSource (0x0205) = uint16(0x0001)
	p += WriteDE_AttrId(p, SDP_ATTR_VENDOR_ID_SOURCE);
	p += WriteDE_Uint16(p, 0x0001);

	// Patch the outer sequence length
	PatchDE_SequenceLength(start, p - start - headerSize);

	return p - start;
}


size_t
SdpServer::_BuildSppRecord(uint8* buf, size_t maxLen, uint8 rfcommChannel)
{
	// Build a DE Sequence for the Serial Port Profile service record
	// (handle 0x00020001)

	uint8* start = buf;
	size_t headerSize = BeginDE_Sequence(buf);
	uint8* p = buf + headerSize;

	// ServiceRecordHandle (0x0000) = uint32(0x00020001)
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_RECORD_HANDLE);
	p += WriteDE_Uint32(p, 0x00020001);

	// ServiceClassIDList (0x0001) = Sequence { UUID16(0x1101) }
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_CLASS_ID_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_SERIAL_PORT);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProtocolDescriptorList (0x0004) =
	//   Sequence { Sequence { UUID16(L2CAP) },
	//              Sequence { UUID16(RFCOMM), uint8(channel) } }
	p += WriteDE_AttrId(p, SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		// L2CAP (no PSM — RFCOMM handles multiplexing)
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_L2CAP);
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}
		// RFCOMM with channel number
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_RFCOMM);
			ip += WriteDE_Uint8(ip, rfcommChannel);
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// BrowseGroupList (0x0005) = Sequence { UUID16(0x1002) }
	p += WriteDE_AttrId(p, SDP_ATTR_BROWSE_GROUP_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_PUBLIC_BROWSE_ROOT);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// LanguageBaseAttributeIDList (0x0006) =
	//   Sequence { uint16(0x656E "en"), uint16(0x006A UTF-8),
	//              uint16(0x0100 base) }
	p += WriteDE_AttrId(p, SDP_ATTR_LANGUAGE_BASE_ATTR_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uint16(sp, 0x656E);  // "en" (ISO 639)
		sp += WriteDE_Uint16(sp, 0x006A);  // UTF-8 (IANA MIBenum 106)
		sp += WriteDE_Uint16(sp, 0x0100);  // attribute ID base
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProfileDescriptorList (0x0009) =
	//   Sequence { Sequence { UUID16(0x1101), uint16(0x0102) } }
	p += WriteDE_AttrId(p, SDP_ATTR_PROFILE_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_SERIAL_PORT);
			ip += WriteDE_Uint16(ip, 0x0102);  // SPP v1.2
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// ServiceName (0x0100) = String("Serial Port")
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_NAME);
	p += WriteDE_String(p, "Serial Port", 11);

	// Patch the outer sequence length
	PatchDE_SequenceLength(start, p - start - headerSize);

	return p - start;
}


size_t
SdpServer::_BuildPbapPceRecord(uint8* buf, size_t maxLen)
{
	// Build a DE Sequence for the PBAP PCE service record
	// (handle 0x00030001)

	uint8* start = buf;
	size_t headerSize = BeginDE_Sequence(buf);
	uint8* p = buf + headerSize;

	// ServiceRecordHandle (0x0000) = uint32(0x00030001)
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_RECORD_HANDLE);
	p += WriteDE_Uint32(p, 0x00030001);

	// ServiceClassIDList (0x0001) = Sequence { UUID16(0x112E) }
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_CLASS_ID_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_PBAP_PCE);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProtocolDescriptorList (0x0004) =
	//   Sequence { Sequence { UUID16(L2CAP) },
	//              Sequence { UUID16(RFCOMM), uint8(0) } }
	// Channel 0 = dynamic (PCE is client-only, doesn't listen)
	p += WriteDE_AttrId(p, SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		// L2CAP (no PSM — RFCOMM handles multiplexing)
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_L2CAP);
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}
		// RFCOMM with channel 0 (dynamic)
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_RFCOMM);
			ip += WriteDE_Uint8(ip, 0);
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// BrowseGroupList (0x0005) = Sequence { UUID16(0x1002) }
	p += WriteDE_AttrId(p, SDP_ATTR_BROWSE_GROUP_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_PUBLIC_BROWSE_ROOT);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// LanguageBaseAttributeIDList (0x0006) =
	//   Sequence { uint16(0x656E "en"), uint16(0x006A UTF-8),
	//              uint16(0x0100 base) }
	p += WriteDE_AttrId(p, SDP_ATTR_LANGUAGE_BASE_ATTR_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uint16(sp, 0x656E);  // "en" (ISO 639)
		sp += WriteDE_Uint16(sp, 0x006A);  // UTF-8 (IANA MIBenum 106)
		sp += WriteDE_Uint16(sp, 0x0100);  // attribute ID base
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProfileDescriptorList (0x0009) =
	//   Sequence { Sequence { UUID16(0x1130), uint16(0x0100) } }
	// PBAP Profile v1.0 — downgraded from 1.2 to avoid Android PSE
	// requiring GOEP 2.0 (L2CAP ERTM) for data transfer operations.
	p += WriteDE_AttrId(p, SDP_ATTR_PROFILE_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_PBAP_PROFILE);
			ip += WriteDE_Uint16(ip, 0x0102);  // PBAP v1.2
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// ServiceName (0x0100) = String("PBAP PCE")
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_NAME);
	p += WriteDE_String(p, "PBAP PCE", 8);

	// Patch the outer sequence length
	PatchDE_SequenceLength(start, p - start - headerSize);

	return p - start;
}


size_t
SdpServer::_BuildOppRecord(uint8* buf, size_t maxLen, uint8 rfcommChannel)
{
	// Build a DE Sequence for the OBEX Object Push service record
	// (handle 0x00040001)

	uint8* start = buf;
	size_t headerSize = BeginDE_Sequence(buf);
	uint8* p = buf + headerSize;

	// ServiceRecordHandle (0x0000) = uint32(0x00040001)
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_RECORD_HANDLE);
	p += WriteDE_Uint32(p, 0x00040001);

	// ServiceClassIDList (0x0001) = Sequence { UUID16(0x1105) }
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_CLASS_ID_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_OBEX_OBJECT_PUSH);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProtocolDescriptorList (0x0004) =
	//   Sequence { Sequence { UUID16(L2CAP) },
	//              Sequence { UUID16(RFCOMM), uint8(channel) },
	//              Sequence { UUID16(OBEX) } }
	p += WriteDE_AttrId(p, SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		// L2CAP
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_L2CAP);
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}
		// RFCOMM with channel number
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_RFCOMM);
			ip += WriteDE_Uint8(ip, rfcommChannel);
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}
		// OBEX (UUID16 0x0008)
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, 0x0008);
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// BrowseGroupList (0x0005) = Sequence { UUID16(0x1002) }
	p += WriteDE_AttrId(p, SDP_ATTR_BROWSE_GROUP_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_PUBLIC_BROWSE_ROOT);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// LanguageBaseAttributeIDList (0x0006) =
	//   Sequence { uint16(0x656E "en"), uint16(0x006A UTF-8),
	//              uint16(0x0100 base) }
	p += WriteDE_AttrId(p, SDP_ATTR_LANGUAGE_BASE_ATTR_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uint16(sp, 0x656E);  // "en"
		sp += WriteDE_Uint16(sp, 0x006A);  // UTF-8
		sp += WriteDE_Uint16(sp, 0x0100);  // attribute ID base
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProfileDescriptorList (0x0009) =
	//   Sequence { Sequence { UUID16(0x1105), uint16(0x0102) } }
	// OPP v1.2
	p += WriteDE_AttrId(p, SDP_ATTR_PROFILE_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_OBEX_OBJECT_PUSH);
			ip += WriteDE_Uint16(ip, 0x0102);  // OPP v1.2
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// ServiceName (0x0100) = String("OBEX Object Push")
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_NAME);
	p += WriteDE_String(p, "OBEX Object Push", 16);

	// SupportedFormatsList (0x0303) =
	//   Sequence { uint8(0x01=vCard2.1), uint8(0x02=vCard3.0),
	//              uint8(0xFF=Any) }
	p += WriteDE_AttrId(p, 0x0303);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uint8(sp, 0x01);  // vCard 2.1
		sp += WriteDE_Uint8(sp, 0x02);  // vCard 3.0
		sp += WriteDE_Uint8(sp, 0xFF);  // Any type
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// Patch the outer sequence length
	PatchDE_SequenceLength(start, p - start - headerSize);

	return p - start;
}


size_t
SdpServer::_BuildA2dpSinkRecord(uint8* buf, size_t maxLen)
{
	// Build a DE Sequence for the A2DP Audio Sink service record
	// (handle 0x00060001)

	uint8* start = buf;
	size_t headerSize = BeginDE_Sequence(buf);
	uint8* p = buf + headerSize;

	// ServiceRecordHandle (0x0000) = uint32(0x00060001)
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_RECORD_HANDLE);
	p += WriteDE_Uint32(p, 0x00060001);

	// ServiceClassIDList (0x0001) = Sequence { UUID16(0x110B) }
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_CLASS_ID_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_A2DP_SINK);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProtocolDescriptorList (0x0004) =
	//   Sequence { Sequence { UUID16(L2CAP), uint16(PSM=0x0019) },
	//              Sequence { UUID16(AVDTP), uint16(0x0103) } }
	p += WriteDE_AttrId(p, SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		// L2CAP with AVDTP PSM
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_L2CAP);
			ip += WriteDE_Uint16(ip, L2CAP_PSM_AVDTP);
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}
		// AVDTP v1.3
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_AVDTP);
			ip += WriteDE_Uint16(ip, 0x0103);  // AVDTP v1.3
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// BrowseGroupList (0x0005) = Sequence { UUID16(0x1002) }
	p += WriteDE_AttrId(p, SDP_ATTR_BROWSE_GROUP_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_PUBLIC_BROWSE_ROOT);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// LanguageBaseAttributeIDList (0x0006)
	p += WriteDE_AttrId(p, SDP_ATTR_LANGUAGE_BASE_ATTR_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uint16(sp, 0x656E);  // "en"
		sp += WriteDE_Uint16(sp, 0x006A);  // UTF-8
		sp += WriteDE_Uint16(sp, 0x0100);  // attribute ID base
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProfileDescriptorList (0x0009) =
	//   Sequence { Sequence { UUID16(0x110D), uint16(0x0103) } }
	// A2DP v1.3
	p += WriteDE_AttrId(p, SDP_ATTR_PROFILE_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, 0x110D);  // A2DP profile UUID
			ip += WriteDE_Uint16(ip, 0x0103);  // A2DP v1.3
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// ServiceName (0x0100) = String("Audio Sink")
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_NAME);
	p += WriteDE_String(p, "Audio Sink", 10);

	// SupportedFeatures (0x0311) = uint16(0x0001)
	// Bit 0: Headphone
	p += WriteDE_AttrId(p, 0x0311);
	p += WriteDE_Uint16(p, 0x0001);

	// Patch the outer sequence length
	PatchDE_SequenceLength(start, p - start - headerSize);

	return p - start;
}


size_t
SdpServer::_BuildA2dpSourceRecord(uint8* buf, size_t maxLen)
{
	// Build a DE Sequence for the A2DP Audio Source service record
	// (handle 0x00070001)

	uint8* start = buf;
	size_t headerSize = BeginDE_Sequence(buf);
	uint8* p = buf + headerSize;

	// ServiceRecordHandle (0x0000) = uint32(0x00070001)
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_RECORD_HANDLE);
	p += WriteDE_Uint32(p, 0x00070001);

	// ServiceClassIDList (0x0001) = Sequence { UUID16(0x110A) }
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_CLASS_ID_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_A2DP_SOURCE);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProtocolDescriptorList (0x0004) =
	//   Sequence { Sequence { UUID16(L2CAP), uint16(PSM=0x0019) },
	//              Sequence { UUID16(AVDTP), uint16(0x0103) } }
	p += WriteDE_AttrId(p, SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		// L2CAP with AVDTP PSM
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_L2CAP);
			ip += WriteDE_Uint16(ip, L2CAP_PSM_AVDTP);
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}
		// AVDTP v1.3
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_AVDTP);
			ip += WriteDE_Uint16(ip, 0x0103);  // AVDTP v1.3
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// BrowseGroupList (0x0005) = Sequence { UUID16(0x1002) }
	p += WriteDE_AttrId(p, SDP_ATTR_BROWSE_GROUP_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_PUBLIC_BROWSE_ROOT);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// LanguageBaseAttributeIDList (0x0006)
	p += WriteDE_AttrId(p, SDP_ATTR_LANGUAGE_BASE_ATTR_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uint16(sp, 0x656E);  // "en"
		sp += WriteDE_Uint16(sp, 0x006A);  // UTF-8
		sp += WriteDE_Uint16(sp, 0x0100);  // attribute ID base
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProfileDescriptorList (0x0009) =
	//   Sequence { Sequence { UUID16(0x110D), uint16(0x0103) } }
	// A2DP v1.3
	p += WriteDE_AttrId(p, SDP_ATTR_PROFILE_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, 0x110D);  // A2DP profile UUID
			ip += WriteDE_Uint16(ip, 0x0103);  // A2DP v1.3
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// ServiceName (0x0100) = String("Audio Source")
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_NAME);
	p += WriteDE_String(p, "Audio Source", 12);

	// SupportedFeatures (0x0311) = uint16(0x0001)
	// Bit 0: Player
	p += WriteDE_AttrId(p, 0x0311);
	p += WriteDE_Uint16(p, 0x0001);

	// Patch the outer sequence length
	PatchDE_SequenceLength(start, p - start - headerSize);

	return p - start;
}


size_t
SdpServer::_BuildHfpRecord(uint8* buf, size_t maxLen, uint8 rfcommChannel)
{
	// Build a DE Sequence for the Hands-Free (HF) service record
	// (handle 0x00050001)

	uint8* start = buf;
	size_t headerSize = BeginDE_Sequence(buf);
	uint8* p = buf + headerSize;

	// ServiceRecordHandle (0x0000) = uint32(0x00050001)
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_RECORD_HANDLE);
	p += WriteDE_Uint32(p, 0x00050001);

	// ServiceClassIDList (0x0001) = Sequence { UUID16(0x111E), UUID16(0x1203) }
	// 0x111E = Handsfree, 0x1203 = GenericAudio
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_CLASS_ID_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_HFP_HF);
		sp += WriteDE_Uuid16(sp, SDP_UUID16_GENERIC_AUDIO);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProtocolDescriptorList (0x0004) =
	//   Sequence { Sequence { UUID16(L2CAP) },
	//              Sequence { UUID16(RFCOMM), uint8(channel) } }
	p += WriteDE_AttrId(p, SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		// L2CAP (no PSM — RFCOMM handles multiplexing)
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_L2CAP);
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}
		// RFCOMM with channel number
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_RFCOMM);
			ip += WriteDE_Uint8(ip, rfcommChannel);
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// BrowseGroupList (0x0005) = Sequence { UUID16(0x1002) }
	p += WriteDE_AttrId(p, SDP_ATTR_BROWSE_GROUP_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_PUBLIC_BROWSE_ROOT);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// LanguageBaseAttributeIDList (0x0006) =
	//   Sequence { uint16(0x656E "en"), uint16(0x006A UTF-8),
	//              uint16(0x0100 base) }
	p += WriteDE_AttrId(p, SDP_ATTR_LANGUAGE_BASE_ATTR_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uint16(sp, 0x656E);  // "en"
		sp += WriteDE_Uint16(sp, 0x006A);  // UTF-8
		sp += WriteDE_Uint16(sp, 0x0100);  // attribute ID base
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProfileDescriptorList (0x0009) =
	//   Sequence { Sequence { UUID16(0x111E), uint16(0x0107) } }
	// HFP v1.7
	p += WriteDE_AttrId(p, SDP_ATTR_PROFILE_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_HFP_HF);
			ip += WriteDE_Uint16(ip, 0x0107);  // HFP v1.7
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// ServiceName (0x0100) = String("Hands-Free")
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_NAME);
	p += WriteDE_String(p, "Hands-Free", 10);

	// SupportedFeatures (0x0311) = uint16(0x001F)
	// Bits: ECNR(0), 3-way(1), CLIP(2), VoiceRecog(3), Volume(4)
	p += WriteDE_AttrId(p, 0x0311);
	p += WriteDE_Uint16(p, 0x001F);

	// Patch the outer sequence length
	PatchDE_SequenceLength(start, p - start - headerSize);

	return p - start;
}


size_t
SdpServer::_BuildAvrcpTargetRecord(uint8* buf, size_t maxLen)
{
	// Build a DE Sequence for the AVRCP Target service record
	// (handle 0x00080001)

	uint8* start = buf;
	size_t headerSize = BeginDE_Sequence(buf);
	uint8* p = buf + headerSize;

	// ServiceRecordHandle (0x0000) = uint32(0x00080001)
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_RECORD_HANDLE);
	p += WriteDE_Uint32(p, 0x00080001);

	// ServiceClassIDList (0x0001) = Sequence { UUID16(0x110C) }
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_CLASS_ID_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_AVRCP_TARGET);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProtocolDescriptorList (0x0004) =
	//   Sequence { Sequence { UUID16(L2CAP), uint16(PSM=0x0017) },
	//              Sequence { UUID16(AVCTP), uint16(0x0104) } }
	p += WriteDE_AttrId(p, SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		// L2CAP with AVCTP PSM
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_L2CAP);
			ip += WriteDE_Uint16(ip, L2CAP_PSM_AVCTP);
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}
		// AVCTP v1.4
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_AVCTP);
			ip += WriteDE_Uint16(ip, 0x0104);  // AVCTP v1.4
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// BrowseGroupList (0x0005) = Sequence { UUID16(0x1002) }
	p += WriteDE_AttrId(p, SDP_ATTR_BROWSE_GROUP_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_PUBLIC_BROWSE_ROOT);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// LanguageBaseAttributeIDList (0x0006)
	p += WriteDE_AttrId(p, SDP_ATTR_LANGUAGE_BASE_ATTR_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uint16(sp, 0x656E);  // "en"
		sp += WriteDE_Uint16(sp, 0x006A);  // UTF-8
		sp += WriteDE_Uint16(sp, 0x0100);  // attribute ID base
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProfileDescriptorList (0x0009) =
	//   Sequence { Sequence { UUID16(0x110E), uint16(0x0106) } }
	// AVRCP v1.6
	p += WriteDE_AttrId(p, SDP_ATTR_PROFILE_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_AVRCP);
			ip += WriteDE_Uint16(ip, 0x0106);  // AVRCP v1.6
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// ServiceName (0x0100) = String("AV Remote Control Target")
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_NAME);
	p += WriteDE_String(p, "AV Remote Control Target", 24);

	// SupportedFeatures (0x0311) = uint16(0x0002)
	// Bit 1: settings (Category 2: Amplifier)
	p += WriteDE_AttrId(p, 0x0311);
	p += WriteDE_Uint16(p, 0x0002);

	// Patch the outer sequence length
	PatchDE_SequenceLength(start, p - start - headerSize);

	return p - start;
}


size_t
SdpServer::_BuildHfpAgRecord(uint8* buf, size_t maxLen, uint8 rfcommChannel)
{
	// Build a DE Sequence for the HFP Audio Gateway service record
	// (handle 0x00090001)

	uint8* start = buf;
	size_t headerSize = BeginDE_Sequence(buf);
	uint8* p = buf + headerSize;

	// ServiceRecordHandle (0x0000) = uint32(0x00090001)
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_RECORD_HANDLE);
	p += WriteDE_Uint32(p, 0x00090001);

	// ServiceClassIDList (0x0001) = Sequence { UUID16(0x111F), UUID16(0x1203) }
	// 0x111F = Handsfree AG, 0x1203 = GenericAudio
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_CLASS_ID_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_HFP_AG);
		sp += WriteDE_Uuid16(sp, SDP_UUID16_GENERIC_AUDIO);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProtocolDescriptorList (0x0004) =
	//   Sequence { Sequence { UUID16(L2CAP) },
	//              Sequence { UUID16(RFCOMM), uint8(channel) } }
	p += WriteDE_AttrId(p, SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		// L2CAP
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_L2CAP);
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}
		// RFCOMM with channel number
		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_RFCOMM);
			ip += WriteDE_Uint8(ip, rfcommChannel);
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// BrowseGroupList (0x0005) = Sequence { UUID16(0x1002) }
	p += WriteDE_AttrId(p, SDP_ATTR_BROWSE_GROUP_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uuid16(sp, SDP_UUID16_PUBLIC_BROWSE_ROOT);
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// LanguageBaseAttributeIDList (0x0006)
	p += WriteDE_AttrId(p, SDP_ATTR_LANGUAGE_BASE_ATTR_LIST);
	{
		uint8* seqStart = p;
		size_t sh = BeginDE_Sequence(p);
		uint8* sp = p + sh;
		sp += WriteDE_Uint16(sp, 0x656E);  // "en"
		sp += WriteDE_Uint16(sp, 0x006A);  // UTF-8
		sp += WriteDE_Uint16(sp, 0x0100);  // attribute ID base
		PatchDE_SequenceLength(seqStart, sp - seqStart - sh);
		p = sp;
	}

	// ProfileDescriptorList (0x0009) =
	//   Sequence { Sequence { UUID16(0x111E), uint16(0x0107) } }
	// HFP v1.7
	p += WriteDE_AttrId(p, SDP_ATTR_PROFILE_DESCRIPTOR_LIST);
	{
		uint8* outerStart = p;
		size_t oh = BeginDE_Sequence(p);
		uint8* op = p + oh;

		{
			uint8* innerStart = op;
			size_t ih = BeginDE_Sequence(op);
			uint8* ip = op + ih;
			ip += WriteDE_Uuid16(ip, SDP_UUID16_HFP_HF);
			ip += WriteDE_Uint16(ip, 0x0107);  // HFP v1.7
			PatchDE_SequenceLength(innerStart, ip - innerStart - ih);
			op = ip;
		}

		PatchDE_SequenceLength(outerStart, op - outerStart - oh);
		p = op;
	}

	// ServiceName (0x0100) = String("Handsfree Audio Gateway")
	p += WriteDE_AttrId(p, SDP_ATTR_SERVICE_NAME);
	p += WriteDE_String(p, "Handsfree Audio Gateway", 23);

	// SupportedFeatures (0x0311) = uint16(0x003F)
	// Bits: 3-way(0), ECNR(1), VoiceRecog(2), InbandRing(3),
	//       VoiceTag(4), Reject(5)
	p += WriteDE_AttrId(p, 0x0311);
	p += WriteDE_Uint16(p, 0x003F);

	// Patch the outer sequence length
	PatchDE_SequenceLength(start, p - start - headerSize);

	return p - start;
}


bool
SdpServer::_RecordMatchesUuid(uint8 recordIndex, const uint16* uuids,
	uint8 count)
{
	const uint16* recordUuids;
	uint8 recordUuidCount;

	switch (recordIndex) {
		case 0:
			recordUuids = fSdpServerUuids;
			recordUuidCount = fSdpServerUuidCount;
			break;
		case 1:
			recordUuids = fPnpInfoUuids;
			recordUuidCount = fPnpInfoUuidCount;
			break;
		case 2:
			recordUuids = fSppUuids;
			recordUuidCount = fSppUuidCount;
			break;
		case 3:
			recordUuids = fPbapPceUuids;
			recordUuidCount = fPbapPceUuidCount;
			break;
		case 4:
			recordUuids = fOppUuids;
			recordUuidCount = fOppUuidCount;
			break;
		case 5:
			recordUuids = fHfpUuids;
			recordUuidCount = fHfpUuidCount;
			break;
		case 6:
			recordUuids = fA2dpSinkUuids;
			recordUuidCount = fA2dpSinkUuidCount;
			break;
		case 7:
			recordUuids = fA2dpSourceUuids;
			recordUuidCount = fA2dpSourceUuidCount;
			break;
		case 8:
			recordUuids = fAvrcpTargetUuids;
			recordUuidCount = fAvrcpTargetUuidCount;
			break;
		case 9:
			recordUuids = fHfpAgUuids;
			recordUuidCount = fHfpAgUuidCount;
			break;
		default:
			return false;
	}

	// A record matches if ALL search UUIDs are found in the record's UUID set
	for (uint8 i = 0; i < count; i++) {
		bool found = false;
		for (uint8 j = 0; j < recordUuidCount; j++) {
			if (uuids[i] == recordUuids[j]) {
				found = true;
				break;
			}
		}
		if (!found)
			return false;
	}

	return true;
}
