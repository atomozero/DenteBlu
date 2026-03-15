/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _SDP_SERVER_H_
#define _SDP_SERVER_H_

#include <SupportDefs.h>


#define SDP_MAX_RECORD_SIZE		128
#define SDP_MAX_UUIDS_PER_RECORD 4


class SdpServer {
public:
							SdpServer();
							~SdpServer();

	ssize_t					HandleRequest(const uint8* request,
								size_t requestLen, uint8* response,
								size_t responseMaxLen);

private:
	// PDU handlers
	ssize_t					_HandleServiceSearchReq(
								uint16 transactionId,
								const uint8* params, size_t paramLen,
								uint8* response, size_t responseMaxLen);
	ssize_t					_HandleServiceAttrReq(
								uint16 transactionId,
								const uint8* params, size_t paramLen,
								uint8* response, size_t responseMaxLen);
	ssize_t					_HandleServiceSearchAttrReq(
								uint16 transactionId,
								const uint8* params, size_t paramLen,
								uint8* response, size_t responseMaxLen);
	ssize_t					_BuildErrorRsp(uint16 transactionId,
								uint16 errorCode, uint8* response,
								size_t responseMaxLen);

	// Data Element parsing
	static bool				_ParseUuidFromDE(const uint8* data, size_t len,
								uint16& uuid, size_t& consumed);
	static bool				_ParseUuidList(const uint8* data, size_t len,
								uint16* uuids, uint8 maxUuids,
								uint8& uuidCount, size_t& consumed);

	// Service record database
	void					_BuildServiceRecords();
	size_t					_BuildSdpServerRecord(uint8* buf, size_t maxLen);
	size_t					_BuildPnpInfoRecord(uint8* buf, size_t maxLen);
	size_t					_BuildSppRecord(uint8* buf, size_t maxLen,
								uint8 rfcommChannel);
	size_t					_BuildPbapPceRecord(uint8* buf,
								size_t maxLen);
	size_t					_BuildOppRecord(uint8* buf, size_t maxLen,
								uint8 rfcommChannel);
	size_t					_BuildHfpRecord(uint8* buf, size_t maxLen,
								uint8 rfcommChannel);
	size_t					_BuildA2dpSinkRecord(uint8* buf,
								size_t maxLen);
	size_t					_BuildA2dpSourceRecord(uint8* buf,
								size_t maxLen);
	size_t					_BuildAvrcpTargetRecord(uint8* buf,
								size_t maxLen);
	size_t					_BuildHfpAgRecord(uint8* buf,
								size_t maxLen,
								uint8 rfcommChannel);
	bool					_RecordMatchesUuid(uint8 recordIndex,
								const uint16* uuids, uint8 count);

	// Pre-built serialized records
	uint8					fSdpServerRecord[SDP_MAX_RECORD_SIZE];
	size_t					fSdpServerRecordLen;
	uint8					fPnpInfoRecord[SDP_MAX_RECORD_SIZE];
	size_t					fPnpInfoRecordLen;
	uint8					fSppRecord[SDP_MAX_RECORD_SIZE];
	size_t					fSppRecordLen;
	uint8					fPbapPceRecord[SDP_MAX_RECORD_SIZE];
	size_t					fPbapPceRecordLen;
	uint8					fOppRecord[SDP_MAX_RECORD_SIZE];
	size_t					fOppRecordLen;
	uint8					fHfpRecord[SDP_MAX_RECORD_SIZE];
	size_t					fHfpRecordLen;
	uint8					fA2dpSinkRecord[SDP_MAX_RECORD_SIZE];
	size_t					fA2dpSinkRecordLen;
	uint8					fA2dpSourceRecord[SDP_MAX_RECORD_SIZE];
	size_t					fA2dpSourceRecordLen;
	uint8					fAvrcpTargetRecord[SDP_MAX_RECORD_SIZE];
	size_t					fAvrcpTargetRecordLen;
	uint8					fHfpAgRecord[SDP_MAX_RECORD_SIZE];
	size_t					fHfpAgRecordLen;

	// UUID lookup arrays per record
	uint16					fSdpServerUuids[SDP_MAX_UUIDS_PER_RECORD];
	uint8					fSdpServerUuidCount;
	uint16					fPnpInfoUuids[SDP_MAX_UUIDS_PER_RECORD];
	uint8					fPnpInfoUuidCount;
	uint16					fSppUuids[SDP_MAX_UUIDS_PER_RECORD];
	uint8					fSppUuidCount;
	uint16					fPbapPceUuids[SDP_MAX_UUIDS_PER_RECORD];
	uint8					fPbapPceUuidCount;
	uint16					fOppUuids[SDP_MAX_UUIDS_PER_RECORD];
	uint8					fOppUuidCount;
	uint16					fHfpUuids[SDP_MAX_UUIDS_PER_RECORD];
	uint8					fHfpUuidCount;
	uint16					fA2dpSinkUuids[SDP_MAX_UUIDS_PER_RECORD];
	uint8					fA2dpSinkUuidCount;
	uint16					fA2dpSourceUuids[SDP_MAX_UUIDS_PER_RECORD];
	uint8					fA2dpSourceUuidCount;
	uint16					fAvrcpTargetUuids[SDP_MAX_UUIDS_PER_RECORD];
	uint8					fAvrcpTargetUuidCount;
	uint16					fHfpAgUuids[SDP_MAX_UUIDS_PER_RECORD];
	uint8					fHfpAgUuidCount;
};


#endif /* _SDP_SERVER_H_ */
