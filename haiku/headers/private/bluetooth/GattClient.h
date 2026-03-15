/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _GATT_CLIENT_H_
#define _GATT_CLIENT_H_

#include <SupportDefs.h>
#include <att.h>


/* UUID types */
#define BT_UUID_16		2
#define BT_UUID_32		4
#define BT_UUID_128		16

typedef struct {
	uint8	type;	/* BT_UUID_16, BT_UUID_32, or BT_UUID_128 */
	union {
		uint16	uuid16;
		uint32	uuid32;
		uint8	uuid128[16];
	};
} bt_uuid_t;


/* GATT Service descriptor */
typedef struct {
	uint16		start_handle;
	uint16		end_handle;
	bt_uuid_t	uuid;
} gatt_service_t;


/* GATT Characteristic descriptor */
typedef struct {
	uint16		handle;			/* declaration handle */
	uint16		value_handle;	/* value attribute handle */
	uint8		properties;		/* GATT_CHAR_PROP_* bitmask */
	bt_uuid_t	uuid;
	uint16		ccc_handle;		/* CCC descriptor handle (0 if not found) */
} gatt_characteristic_t;


/* Maximum services/characteristics per discovery */
#define GATT_MAX_SERVICES			32
#define GATT_MAX_CHARACTERISTICS	64


class AttChannel;

class GattClient {
public:
								GattClient(AttChannel* channel);
	virtual						~GattClient();

	status_t					ExchangeMtu(uint16 desiredMtu);

	status_t					DiscoverPrimaryServices(
									gatt_service_t* _services,
									uint16* _count);
	status_t					DiscoverPrimaryServiceByUuid(
									const bt_uuid_t& uuid,
									gatt_service_t* _service);
	status_t					DiscoverCharacteristics(
									uint16 startHandle,
									uint16 endHandle,
									gatt_characteristic_t* _chars,
									uint16* _count);
	status_t					DiscoverDescriptors(
									uint16 startHandle,
									uint16 endHandle,
									gatt_characteristic_t* _char);

	status_t					ReadCharacteristic(uint16 handle,
									uint8* _value, uint16* _length);
	status_t					WriteCharacteristic(uint16 handle,
									const uint8* value, uint16 length);
	status_t					WriteCharacteristicNoResponse(
									uint16 handle, const uint8* value,
									uint16 length);

	status_t					SubscribeNotifications(uint16 cccHandle,
									bool enable);

private:
	static void					_ParseUuid(const uint8* data,
									uint8 length, bt_uuid_t* _uuid);
	static bool					_UuidEquals(const bt_uuid_t& a,
									const bt_uuid_t& b);

	AttChannel*					fChannel;
};


#endif /* _GATT_CLIENT_H_ */
