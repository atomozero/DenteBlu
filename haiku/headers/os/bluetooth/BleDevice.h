/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _BLE_DEVICE_H_
#define _BLE_DEVICE_H_

#include <Messenger.h>
#include <String.h>

#include <bluetooth/bluetooth.h>


namespace Bluetooth {


class BleDevice {
public:
								BleDevice();
								BleDevice(const bdaddr_t& address,
									uint8 addressType = 0);
	virtual						~BleDevice();

	/* Scanning */
	static status_t				StartScan(uint8 scanType = 0x01,
									uint16 interval = 0x0060,
									uint16 window = 0x0030,
									uint8 filterDup = 0x01);
	static status_t				StopScan();

	/* Connection */
	status_t					Connect();
	status_t					Disconnect();
	bool						IsConnected() const;

	/* Pairing */
	status_t					Pair(uint32 passkey);

	/* GATT Operations */
	status_t					DiscoverServices();
	status_t					ReadCharacteristic(uint16 handle,
									uint8* _value, uint16* _length);
	status_t					WriteCharacteristic(uint16 handle,
									const uint8* value, uint16 length);
	status_t					SubscribeNotifications(uint16 handle,
									bool enable);

	/* Accessors */
	const bdaddr_t&				Address() const { return fAddress; }
	uint8						AddressType() const { return fAddressType; }
	uint16						ConnectionHandle() const
									{ return fConnectionHandle; }
	void						SetConnectionHandle(uint16 handle)
									{ fConnectionHandle = handle; }

private:
	static BMessenger			_ServerMessenger();

	bdaddr_t					fAddress;
	uint8						fAddressType;
	uint16						fConnectionHandle;
	bool						fConnected;
};


} /* namespace Bluetooth */


#endif /* _BLE_DEVICE_H_ */
