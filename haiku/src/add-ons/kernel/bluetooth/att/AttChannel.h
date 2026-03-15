/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _ATT_CHANNEL_H_
#define _ATT_CHANNEL_H_

#include <OS.h>

#include <bluetooth/bluetooth.h>
#include <btCoreData.h>
#include <att.h>


#define ATT_RESPONSE_TIMEOUT		30000000	/* 30 seconds in microseconds */
#define ATT_MAX_PDU_SIZE			512

typedef void (*att_notification_callback)(uint16 handle, const uint8* data,
	uint16 length, void* cookie);


class AttChannel {
public:
								AttChannel(HciConnection* connection);
	virtual						~AttChannel();

	status_t					ReceiveData(net_buffer* buffer);

	status_t					ExchangeMtu(uint16 clientMtu,
									uint16* _serverMtu);
	status_t					FindInformation(uint16 startHandle,
									uint16 endHandle, uint8* _response,
									uint16* _responseLength);
	status_t					ReadByGroupType(uint16 startHandle,
									uint16 endHandle, const uint8* uuid,
									uint8 uuidLength, uint8* _response,
									uint16* _responseLength);
	status_t					ReadByType(uint16 startHandle,
									uint16 endHandle, const uint8* uuid,
									uint8 uuidLength, uint8* _response,
									uint16* _responseLength);
	status_t					ReadAttribute(uint16 handle,
									uint8* _value, uint16* _valueLength);
	status_t					WriteAttribute(uint16 handle,
									const uint8* value, uint16 valueLength);
	status_t					WriteCommand(uint16 handle,
									const uint8* value, uint16 valueLength);

	status_t					EnableNotifications(uint16 cccHandle,
									bool enable);
	void						SetNotificationCallback(
									att_notification_callback callback,
									void* cookie);

	uint16						Mtu() const { return fMtu; }

private:
	status_t					_SendPdu(const uint8* pdu, uint16 length);
	status_t					_WaitForResponse(uint8 expectedOpcode,
									uint8* _response,
									uint16* _responseLength);

	HciConnection*				fConnection;
	uint16						fMtu;

	sem_id						fResponseSem;
	uint8						fResponseBuffer[ATT_MAX_PDU_SIZE];
	uint16						fResponseLength;
	uint8						fExpectedOpcode;

	att_notification_callback	fNotificationCallback;
	void*						fNotificationCookie;
};


#endif /* _ATT_CHANNEL_H_ */
