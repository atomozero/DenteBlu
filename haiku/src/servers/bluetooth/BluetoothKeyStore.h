/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _BLUETOOTH_KEY_STORE_H
#define _BLUETOOTH_KEY_STORE_H

#include <Message.h>
#include <String.h>
#include <bluetooth/bluetooth.h>


class BluetoothKeyStore {
public:
								BluetoothKeyStore();

	status_t					Load(const char* path = NULL);
	status_t					Save(const char* path = NULL);

	// Classic link keys
	status_t					AddLinkKey(const bdaddr_t& addr,
									const linkkey_t& key, uint8 type);
	bool						FindLinkKey(const bdaddr_t& addr,
									linkkey_t* outKey, uint8* outType);
	void						RemoveLinkKey(const bdaddr_t& addr);

	// Device names
	status_t					AddDeviceName(const bdaddr_t& addr,
									const char* name);
	bool						FindDeviceName(const bdaddr_t& addr,
									BString* outName);

	// Device class (CoD)
	status_t					AddDeviceClass(const bdaddr_t& addr,
									uint32 cod);
	bool						FindDeviceClass(const bdaddr_t& addr,
									uint32* outCod);

	// LE Long-Term Keys
	status_t					AddLtk(const bdaddr_t& addr,
									const uint8 ltk[16], uint16 ediv,
									const uint8 rand[8]);
	bool						FindLtk(const bdaddr_t& addr,
									uint8 outLtk[16], uint16* outEdiv,
									uint8 outRand[8]);
	void						RemoveLtk(const bdaddr_t& addr);

	// Auto-reconnect flag per device
	status_t					SetAutoReconnect(const bdaddr_t& addr,
									bool enabled);
	bool						HasAutoReconnect(const bdaddr_t& addr) const;

	// Enumerate paired devices (those with link keys)
	status_t					GetPairedDevices(bdaddr_t* outAddrs,
									uint8 maxDevices,
									uint8* outCount) const;

	// For enumeration (e.g. by CLI tools)
	const BMessage&				Keys() const { return fKeys; }

private:
	static	BString				_AddrToString(const bdaddr_t& addr);
	static	BString				_LinkKeyName(const bdaddr_t& addr);
	static	BString				_LtkName(const bdaddr_t& addr);
	static	BString				_DeviceNameKey(const bdaddr_t& addr);
	static	BString				_DeviceClassKey(const bdaddr_t& addr);
	static	BString				_AutoReconnectKey(const bdaddr_t& addr);
			status_t			_DefaultPath(BString& outPath);

	BMessage					fKeys;
	BString						fDefaultPath;
};


#endif /* _BLUETOOTH_KEY_STORE_H */
