/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * Nordic UART Service (NUS) client for BLE communication with
 * MeshCore firmware on Heltec V3 devices.
 */
#ifndef _NUS_CLIENT_H_
#define _NUS_CLIENT_H_

#include <OS.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/BleDevice.h>


namespace Bluetooth {


/*
 * Nordic UART Service UUIDs (128-bit, little-endian byte order).
 *
 * Service:  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 * RX Char:  6E400002-B5A3-F393-E0A9-E50E24DCCA9E  (write)
 * TX Char:  6E400003-B5A3-F393-E0A9-E50E24DCCA9E  (notify)
 */
static const uint8 kNusServiceUuid[16] = {
	0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
	0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E
};

static const uint8 kNusRxCharUuid[16] = {
	0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
	0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E
};

static const uint8 kNusTxCharUuid[16] = {
	0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
	0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E
};


/* MeshCore NUS parameters */
#define NUS_MAX_FRAME_SIZE		172
#define NUS_MIN_WRITE_INTERVAL	60000	/* microseconds (60ms) */


/* Data received callback */
typedef void (*nus_data_callback)(const uint8* data, uint16 length,
	void* cookie);


class NusClient {
public:
								NusClient();
	virtual						~NusClient();

	/* Full initialization: connect, pair, discover NUS, subscribe */
	status_t					Initialize(const bdaddr_t& address,
									uint8 addressType, uint32 passkey);

	/* Send data to the device (via NUS RX characteristic) */
	status_t					Send(const uint8* data, uint16 length);

	/* Register callback for data received via TX notifications */
	void						SetDataCallback(nus_data_callback callback,
									void* cookie);

	/* Shutdown and disconnect */
	void						Shutdown();

	bool						IsInitialized() const
									{ return fInitialized; }

private:
	BleDevice					fDevice;
	bool						fInitialized;

	uint16						fRxCharHandle;
	uint16						fTxCharHandle;
	uint16						fTxCccHandle;

	bigtime_t					fLastWriteTime;

	nus_data_callback			fDataCallback;
	void*						fDataCookie;
};


} /* namespace Bluetooth */


#endif /* _NUS_CLIENT_H_ */
