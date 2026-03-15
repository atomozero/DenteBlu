/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <bluetooth/BleDevice.h>

#include <string.h>

#include <Message.h>
#include <Messenger.h>
#include <Roster.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetoothserver_p.h>


namespace Bluetooth {


BleDevice::BleDevice()
	:
	fAddressType(0),
	fConnectionHandle(0),
	fConnected(false)
{
	memset(&fAddress, 0, sizeof(bdaddr_t));
}


BleDevice::BleDevice(const bdaddr_t& address, uint8 addressType)
	:
	fAddressType(addressType),
	fConnectionHandle(0),
	fConnected(false)
{
	bdaddrUtils::Copy(fAddress, address);
}


BleDevice::~BleDevice()
{
}


/* static */ status_t
BleDevice::StartScan(uint8 scanType, uint16 interval, uint16 window,
	uint8 filterDup)
{
	BMessenger messenger = _ServerMessenger();
	if (!messenger.IsValid())
		return B_ERROR;

	BMessage request(BT_MSG_LE_SCAN_START);
	request.AddInt8("scan_type", scanType);
	request.AddInt16("interval", interval);
	request.AddInt16("window", window);
	request.AddInt8("filter_dup", filterDup);

	BMessage reply;
	status_t status = messenger.SendMessage(&request, &reply);
	if (status != B_OK)
		return status;

	int32 result;
	if (reply.FindInt32("status", &result) == B_OK)
		return (status_t)result;

	return B_OK;
}


/* static */ status_t
BleDevice::StopScan()
{
	BMessenger messenger = _ServerMessenger();
	if (!messenger.IsValid())
		return B_ERROR;

	BMessage request(BT_MSG_LE_SCAN_STOP);
	BMessage reply;
	status_t status = messenger.SendMessage(&request, &reply);
	if (status != B_OK)
		return status;

	int32 result;
	if (reply.FindInt32("status", &result) == B_OK)
		return (status_t)result;

	return B_OK;
}


status_t
BleDevice::Connect()
{
	BMessenger messenger = _ServerMessenger();
	if (!messenger.IsValid())
		return B_ERROR;

	BMessage request(BT_MSG_LE_CONNECT);
	request.AddData("bdaddr", B_ANY_TYPE, &fAddress, sizeof(bdaddr_t));
	request.AddInt8("address_type", fAddressType);

	BMessage reply;
	status_t status = messenger.SendMessage(&request, &reply);
	if (status != B_OK)
		return status;

	int8 btStatus;
	if (reply.FindInt8("status", &btStatus) == B_OK && btStatus == 0) {
		int16 handle;
		if (reply.FindInt16("handle", &handle) == B_OK) {
			fConnectionHandle = handle;
			fConnected = true;
		}
	}

	return status;
}


status_t
BleDevice::Disconnect()
{
	if (!fConnected)
		return B_NOT_ALLOWED;

	BMessenger messenger = _ServerMessenger();
	if (!messenger.IsValid())
		return B_ERROR;

	BMessage request(BT_MSG_LE_DISCONNECT);
	request.AddInt16("handle", fConnectionHandle);

	BMessage reply;
	status_t status = messenger.SendMessage(&request, &reply);

	fConnected = false;
	fConnectionHandle = 0;

	return status;
}


bool
BleDevice::IsConnected() const
{
	return fConnected;
}


status_t
BleDevice::Pair(uint32 passkey)
{
	if (!fConnected)
		return B_NOT_ALLOWED;

	BMessenger messenger = _ServerMessenger();
	if (!messenger.IsValid())
		return B_ERROR;

	BMessage request(BT_MSG_LE_PAIR);
	request.AddInt16("handle", fConnectionHandle);
	request.AddInt32("passkey", passkey);

	BMessage reply;
	return messenger.SendMessage(&request, &reply);
}


status_t
BleDevice::DiscoverServices()
{
	if (!fConnected)
		return B_NOT_ALLOWED;

	BMessenger messenger = _ServerMessenger();
	if (!messenger.IsValid())
		return B_ERROR;

	BMessage request(BT_MSG_LE_GATT_DISCOVER);
	request.AddInt16("handle", fConnectionHandle);

	BMessage reply;
	return messenger.SendMessage(&request, &reply);
}


status_t
BleDevice::ReadCharacteristic(uint16 handle, uint8* _value, uint16* _length)
{
	if (!fConnected)
		return B_NOT_ALLOWED;

	BMessenger messenger = _ServerMessenger();
	if (!messenger.IsValid())
		return B_ERROR;

	BMessage request(BT_MSG_LE_GATT_READ);
	request.AddInt16("conn_handle", fConnectionHandle);
	request.AddInt16("attr_handle", handle);

	BMessage reply;
	status_t status = messenger.SendMessage(&request, &reply);
	if (status != B_OK)
		return status;

	const void* data;
	ssize_t size;
	if (reply.FindData("value", B_ANY_TYPE, &data, &size) == B_OK) {
		memcpy(_value, data, size);
		*_length = (uint16)size;
	}

	return B_OK;
}


status_t
BleDevice::WriteCharacteristic(uint16 handle, const uint8* value,
	uint16 length)
{
	if (!fConnected)
		return B_NOT_ALLOWED;

	BMessenger messenger = _ServerMessenger();
	if (!messenger.IsValid())
		return B_ERROR;

	BMessage request(BT_MSG_LE_GATT_WRITE);
	request.AddInt16("conn_handle", fConnectionHandle);
	request.AddInt16("attr_handle", handle);
	request.AddData("value", B_ANY_TYPE, value, length);

	BMessage reply;
	return messenger.SendMessage(&request, &reply);
}


status_t
BleDevice::SubscribeNotifications(uint16 handle, bool enable)
{
	if (!fConnected)
		return B_NOT_ALLOWED;

	BMessenger messenger = _ServerMessenger();
	if (!messenger.IsValid())
		return B_ERROR;

	BMessage request(BT_MSG_LE_GATT_SUBSCRIBE);
	request.AddInt16("conn_handle", fConnectionHandle);
	request.AddInt16("attr_handle", handle);
	request.AddBool("enable", enable);

	BMessage reply;
	return messenger.SendMessage(&request, &reply);
}


/* static */ BMessenger
BleDevice::_ServerMessenger()
{
	return BMessenger(BLUETOOTH_SIGNATURE);
}


} /* namespace Bluetooth */
