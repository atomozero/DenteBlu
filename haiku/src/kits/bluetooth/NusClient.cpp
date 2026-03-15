/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <bluetooth/NusClient.h>

#include <string.h>

#include <OS.h>

#include <bluetooth/bdaddrUtils.h>


namespace Bluetooth {


NusClient::NusClient()
	:
	fInitialized(false),
	fRxCharHandle(0),
	fTxCharHandle(0),
	fTxCccHandle(0),
	fLastWriteTime(0),
	fDataCallback(NULL),
	fDataCookie(NULL)
{
}


NusClient::~NusClient()
{
	Shutdown();
}


status_t
NusClient::Initialize(const bdaddr_t& address, uint8 addressType,
	uint32 passkey)
{
	if (fInitialized)
		return B_NOT_ALLOWED;

	fDevice = BleDevice(address, addressType);

	/* Step 1: Connect */
	status_t status = fDevice.Connect();
	if (status != B_OK)
		return status;

	/* Step 2: Pair with passkey */
	status = fDevice.Pair(passkey);
	if (status != B_OK) {
		fDevice.Disconnect();
		return status;
	}

	/* Step 3: Discover services and find NUS.
	   The actual GATT discovery would be handled by the server side.
	   Here we request it and expect the handles to be returned. */
	status = fDevice.DiscoverServices();
	if (status != B_OK) {
		fDevice.Disconnect();
		return status;
	}

	/* Step 4: Subscribe to TX notifications.
	   In a full implementation, after discovery we would have the
	   TX characteristic CCC handle. For now, mark as initialized
	   and the handles will be set by the discovery process. */

	fInitialized = true;
	fLastWriteTime = 0;

	return B_OK;
}


status_t
NusClient::Send(const uint8* data, uint16 length)
{
	if (!fInitialized || fRxCharHandle == 0)
		return B_NOT_ALLOWED;

	/* Enforce maximum frame size */
	if (length > NUS_MAX_FRAME_SIZE)
		return B_BAD_VALUE;

	/* Rate limiting: minimum 60ms between writes */
	bigtime_t now = system_time();
	bigtime_t elapsed = now - fLastWriteTime;
	if (elapsed < NUS_MIN_WRITE_INTERVAL && fLastWriteTime != 0) {
		snooze(NUS_MIN_WRITE_INTERVAL - elapsed);
	}

	status_t status = fDevice.WriteCharacteristic(fRxCharHandle, data,
		length);
	fLastWriteTime = system_time();

	return status;
}


void
NusClient::SetDataCallback(nus_data_callback callback, void* cookie)
{
	fDataCallback = callback;
	fDataCookie = cookie;
}


void
NusClient::Shutdown()
{
	if (!fInitialized)
		return;

	/* Unsubscribe from TX notifications */
	if (fTxCccHandle != 0)
		fDevice.SubscribeNotifications(fTxCccHandle, false);

	fDevice.Disconnect();

	fInitialized = false;
	fRxCharHandle = 0;
	fTxCharHandle = 0;
	fTxCccHandle = 0;
}


} /* namespace Bluetooth */
