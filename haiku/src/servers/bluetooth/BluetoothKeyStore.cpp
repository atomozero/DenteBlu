/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "BluetoothKeyStore.h"

#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>
#include <String.h>

#include <stdio.h>
#include <string.h>


// Packed struct for link key storage: linkkey_t (16 bytes) + type (1 byte)
struct link_key_record {
	uint8	key[16];
	uint8	type;
} __attribute__((packed));

// Packed struct for LTK storage: ltk (16) + ediv (2) + rand (8)
struct ltk_record {
	uint8	ltk[16];
	uint16	ediv;
	uint8	rand[8];
} __attribute__((packed));


BluetoothKeyStore::BluetoothKeyStore()
{
}


status_t
BluetoothKeyStore::_DefaultPath(BString& outPath)
{
	if (fDefaultPath.Length() > 0) {
		outPath = fDefaultPath;
		return B_OK;
	}

	BPath path;
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status != B_OK)
		return status;

	status = path.Append("system/bluetooth");
	if (status != B_OK)
		return status;

	// Ensure directory exists
	BDirectory dir;
	status = dir.SetTo(path.Path());
	if (status == B_ENTRY_NOT_FOUND) {
		status = create_directory(path.Path(), 0755);
		if (status != B_OK)
			return status;
	} else if (status != B_OK)
		return status;

	status = path.Append("keys");
	if (status != B_OK)
		return status;

	fDefaultPath = path.Path();
	outPath = fDefaultPath;
	return B_OK;
}


status_t
BluetoothKeyStore::Load(const char* path)
{
	BString filePath;
	if (path != NULL)
		filePath = path;
	else {
		status_t status = _DefaultPath(filePath);
		if (status != B_OK)
			return status;
	}

	BFile file(filePath.String(), B_READ_ONLY);
	status_t status = file.InitCheck();
	if (status != B_OK)
		return status;

	BMessage loaded;
	status = loaded.Unflatten(&file);
	if (status != B_OK)
		return status;

	fKeys = loaded;
	return B_OK;
}


status_t
BluetoothKeyStore::Save(const char* path)
{
	BString filePath;
	if (path != NULL)
		filePath = path;
	else {
		status_t status = _DefaultPath(filePath);
		if (status != B_OK)
			return status;
	}

	BFile file(filePath.String(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	status_t status = file.InitCheck();
	if (status != B_OK)
		return status;

	return fKeys.Flatten(&file);
}


BString
BluetoothKeyStore::_AddrToString(const bdaddr_t& addr)
{
	BString str;
	str.SetToFormat("%02X:%02X:%02X:%02X:%02X:%02X",
		addr.b[5], addr.b[4], addr.b[3],
		addr.b[2], addr.b[1], addr.b[0]);
	return str;
}


BString
BluetoothKeyStore::_LinkKeyName(const bdaddr_t& addr)
{
	BString name("lk:");
	name.Append(_AddrToString(addr));
	return name;
}


BString
BluetoothKeyStore::_LtkName(const bdaddr_t& addr)
{
	BString name("ltk:");
	name.Append(_AddrToString(addr));
	return name;
}


BString
BluetoothKeyStore::_DeviceNameKey(const bdaddr_t& addr)
{
	BString name("dn:");
	name.Append(_AddrToString(addr));
	return name;
}


BString
BluetoothKeyStore::_DeviceClassKey(const bdaddr_t& addr)
{
	BString name("dc:");
	name.Append(_AddrToString(addr));
	return name;
}


status_t
BluetoothKeyStore::AddLinkKey(const bdaddr_t& addr, const linkkey_t& key,
	uint8 type)
{
	struct link_key_record record;
	memcpy(record.key, key.l, 16);
	record.type = type;

	BString name = _LinkKeyName(addr);

	// Remove existing entry if any
	fKeys.RemoveName(name.String());

	return fKeys.AddData(name.String(), B_RAW_TYPE, &record, sizeof(record));
}


bool
BluetoothKeyStore::FindLinkKey(const bdaddr_t& addr, linkkey_t* outKey,
	uint8* outType)
{
	BString name = _LinkKeyName(addr);

	const void* data;
	ssize_t size;
	if (fKeys.FindData(name.String(), B_RAW_TYPE, &data, &size) != B_OK)
		return false;

	if (size != sizeof(struct link_key_record))
		return false;

	const struct link_key_record* record = (const struct link_key_record*)data;
	if (outKey != NULL)
		memcpy(outKey->l, record->key, 16);
	if (outType != NULL)
		*outType = record->type;

	return true;
}


void
BluetoothKeyStore::RemoveLinkKey(const bdaddr_t& addr)
{
	BString name = _LinkKeyName(addr);
	fKeys.RemoveName(name.String());
}


status_t
BluetoothKeyStore::AddDeviceName(const bdaddr_t& addr, const char* name)
{
	BString key = _DeviceNameKey(addr);
	fKeys.RemoveName(key.String());
	return fKeys.AddString(key.String(), name);
}


bool
BluetoothKeyStore::FindDeviceName(const bdaddr_t& addr, BString* outName)
{
	BString key = _DeviceNameKey(addr);
	return fKeys.FindString(key.String(), outName) == B_OK;
}


status_t
BluetoothKeyStore::AddDeviceClass(const bdaddr_t& addr, uint32 cod)
{
	BString key = _DeviceClassKey(addr);
	fKeys.RemoveName(key.String());
	return fKeys.AddInt32(key.String(), (int32)cod);
}


bool
BluetoothKeyStore::FindDeviceClass(const bdaddr_t& addr, uint32* outCod)
{
	BString key = _DeviceClassKey(addr);
	int32 val;
	if (fKeys.FindInt32(key.String(), &val) != B_OK)
		return false;
	if (outCod != NULL)
		*outCod = (uint32)val;
	return true;
}


status_t
BluetoothKeyStore::AddLtk(const bdaddr_t& addr, const uint8 ltk[16],
	uint16 ediv, const uint8 rand[8])
{
	struct ltk_record record;
	memcpy(record.ltk, ltk, 16);
	record.ediv = ediv;
	memcpy(record.rand, rand, 8);

	BString name = _LtkName(addr);

	// Remove existing entry if any
	fKeys.RemoveName(name.String());

	return fKeys.AddData(name.String(), B_RAW_TYPE, &record, sizeof(record));
}


bool
BluetoothKeyStore::FindLtk(const bdaddr_t& addr, uint8 outLtk[16],
	uint16* outEdiv, uint8 outRand[8])
{
	BString name = _LtkName(addr);

	const void* data;
	ssize_t size;
	if (fKeys.FindData(name.String(), B_RAW_TYPE, &data, &size) != B_OK)
		return false;

	if (size != sizeof(struct ltk_record))
		return false;

	const struct ltk_record* record = (const struct ltk_record*)data;
	if (outLtk != NULL)
		memcpy(outLtk, record->ltk, 16);
	if (outEdiv != NULL)
		*outEdiv = record->ediv;
	if (outRand != NULL)
		memcpy(outRand, record->rand, 8);

	return true;
}


void
BluetoothKeyStore::RemoveLtk(const bdaddr_t& addr)
{
	BString name = _LtkName(addr);
	fKeys.RemoveName(name.String());
}


BString
BluetoothKeyStore::_AutoReconnectKey(const bdaddr_t& addr)
{
	BString name("ar:");
	name.Append(_AddrToString(addr));
	return name;
}


status_t
BluetoothKeyStore::SetAutoReconnect(const bdaddr_t& addr, bool enabled)
{
	BString key = _AutoReconnectKey(addr);
	fKeys.RemoveName(key.String());
	return fKeys.AddBool(key.String(), enabled);
}


bool
BluetoothKeyStore::HasAutoReconnect(const bdaddr_t& addr) const
{
	BString key = _AutoReconnectKey(addr);
	bool value = false;
	if (fKeys.FindBool(key.String(), &value) != B_OK)
		return false;
	return value;
}


status_t
BluetoothKeyStore::GetPairedDevices(bdaddr_t* outAddrs, uint8 maxDevices,
	uint8* outCount) const
{
	*outCount = 0;

	char* name;
	type_code type;
	int32 countFound;

	for (int32 i = 0;
		fKeys.GetInfo(B_RAW_TYPE, i, &name, &type, &countFound) == B_OK;
		i++) {
		if (strncmp(name, "lk:", 3) != 0)
			continue;

		if (*outCount >= maxDevices)
			break;

		unsigned int b[6];
		if (sscanf(name + 3, "%02X:%02X:%02X:%02X:%02X:%02X",
				&b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
			continue;

		bdaddr_t addr;
		addr.b[5] = (uint8)b[0];
		addr.b[4] = (uint8)b[1];
		addr.b[3] = (uint8)b[2];
		addr.b[2] = (uint8)b[3];
		addr.b[1] = (uint8)b[4];
		addr.b[0] = (uint8)b[5];

		outAddrs[*outCount] = addr;
		(*outCount)++;
	}

	return B_OK;
}
