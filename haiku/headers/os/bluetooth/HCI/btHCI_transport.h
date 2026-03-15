/*
 * Copyright 2007 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _BTHCI_TRANSPORT_H_
#define _BTHCI_TRANSPORT_H_


#include <bluetooth/HCI/btHCI.h>

#include <Drivers.h>
#include <sys/socket.h>
#include <util/DoublyLinkedList.h>

#ifdef _KERNEL_MODE
#include <net_buffer.h>
#endif


/* Here the transport driver have some flags that
 * can be used to inform the upper layer about some
 * special behaouvior to perform */

#define BT_IGNORE_THIS_DEVICE	(1 << 0)
#define BT_SCO_NOT_WORKING		(1 << 1)
#define BT_WILL_NEED_A_RESET	(1 << 2)
#define BT_DIGIANSWER			(1 << 4)

// Mandatory IOCTLS
#define BT_IOCTLS_OFFSET 3000

enum {
	ISSUE_BT_COMMAND = B_DEVICE_OP_CODES_END + BT_IOCTLS_OFFSET, // 12999
	GET_STATS,
	GET_NOTIFICATION_PORT,
	GET_HCI_ID,
	BT_UP
};

// To deprecate ...
#define PACK_PORTCODE(type,hid,data) ((type & 0xFF) << 24 | (hid & 0xFF) << 16 | (data & 0xFFFF))
#define GET_PORTCODE_TYPE(code) ((code & 0xFF000000) >> 24)
#define GET_PORTCODE_HID(code) ((code & 0x00FF0000) >> 16)
#define GET_PORTCODE_DATA(code) ((code & 0x0000FFFF))

/*  Port drivers can use to send information (1 for all for
	at moment refer to ioctl GET_NOTIFICATION_PORT)*/
#define BT_USERLAND_PORT_NAME "BT Kernel-User Event"
#define BT_RX_PORT_NAME "BT Kernel RX assembly"
#define BLUETOOTH_CONNECTION_PORT "bluetooth connection port"


typedef enum {
	ANCILLYANT = (1<<0),
	RUNNING = (1<<1),
	LEAVING = (1<<2),
	SENDING = (1<<3),
	PROCESSING = (1<<4)
} bt_transport_status_t;


typedef uint8 bt_stat_t;
typedef struct bt_hci_statistics {
	bt_stat_t acceptedTX;
	bt_stat_t rejectedTX;
	bt_stat_t successfulTX;
	bt_stat_t errorTX;

	bt_stat_t acceptedRX;
	bt_stat_t rejectedRX;
	bt_stat_t successfulRX;
	bt_stat_t errorRX;

	bt_stat_t commandTX;
	bt_stat_t eventRX;
	bt_stat_t aclTX;
	bt_stat_t aclRX;
	bt_stat_t scoTX;
	bt_stat_t scoRX;
	bt_stat_t escoTX;
	bt_stat_t escoRX;

	bt_stat_t bytesRX;
	bt_stat_t bytesTX;
} bt_hci_statistics;


typedef struct bt_hci_device {
	transport_type	kind;
	char			realName[B_OS_NAME_LENGTH];
} bt_hci_device;


#if defined(_KERNEL_MODE)
/* Hooks which drivers will have to provide.
 * The structure is meant to be allocated in driver side and
 * provided to the HCI where it will fill the remaining fields
 */
typedef struct bt_hci_transport_hooks {
	// to be filled by driver
	status_t	(*SendCommand)(hci_id hciId, void* command);
	status_t	(*SendACL)(hci_id hciId, net_buffer* nbuf);
	status_t	(*SendSCO)(hci_id hciId, net_buffer* nbuf);
	status_t	(*SendESCO)(hci_id hciId, net_buffer* nbuf);

	status_t	(*DeliverStatistics)(hci_id hciId, bt_hci_statistics* statistics);

	transport_type kind;
} bt_hci_transport_hooks;

typedef struct bt_hci_device_information {
	uint32	flags;
	uint16	vendorId;
	uint16	deviceId;
	char	name[B_OS_NAME_LENGTH];
} bt_hci_device_information;


#if defined(__cplusplus)
struct bluetooth_device : DoublyLinkedListLinkImpl<bluetooth_device> {

	net_buffer*	fBuffersRx[HCI_NUM_PACKET_TYPES];
	size_t		fExpectedPacketSize[HCI_NUM_PACKET_TYPES];
	hci_id		index;

	uint16		supportedPacketTypes;
	uint16		linkMode;
	int			fd;

	bt_hci_device_information*	info;
	bt_hci_transport_hooks*		hooks;
	uint16						mtu;

};
#else
struct bluetooth_device;
#endif


#define BT_HCI_MODULE_NAME "bluetooth/hci/v1"

// Possible definition of a bus manager
typedef struct bt_hci_module_info {
	module_info info;
	// Registration in Stack
	status_t			(*RegisterDriver)(bt_hci_transport_hooks* hooks,
							bluetooth_device** device);
	status_t			(*UnregisterDriver)(hci_id id);
	bluetooth_device*	(*FindDeviceByID)(hci_id id);

	// to be called from transport driver
	status_t			(*PostTransportPacket)(hci_id hid, bt_packet_t type,
							void* data, size_t count);

	// To be called from upper layers
	status_t		(*PostACL)(hci_id hciId, net_buffer* buffer);
	status_t		(*PostSCO)(hci_id hciId, net_buffer* buffer);
	status_t		(*PostESCO)(hci_id hciId, net_buffer* buffer);

	// LE GATT ioctl dispatch (server -> kernel)
	status_t		(*HandleLeGattIoctl)(hci_id hciId, uint32 op,
						void* data, size_t size);

} bt_hci_module_info ;

#endif

// LE GATT ioctls (server -> kernel)
enum {
	BT_LE_GATT_EXCHANGE_MTU = BT_UP + 1,			// 13004
	BT_LE_GATT_DISCOVER_SERVICES,					// 13005
	BT_LE_GATT_DISCOVER_CHARACTERISTICS,			// 13006
	BT_LE_GATT_DISCOVER_DESCRIPTORS,				// 13007
	BT_LE_GATT_READ,								// 13008
	BT_LE_GATT_WRITE,								// 13009
	BT_LE_GATT_WRITE_NO_RESPONSE,					// 13010
	BT_LE_GATT_SUBSCRIBE,							// 13011
	BT_LE_SMP_INITIATE_PAIRING,						// 13012
	BT_LE_SMP_NC_CONFIRM,							// 13013
};

/* GATT ioctl parameter structs — shared between kernel and userspace. */
#include <GattClient.h>

#define BT_GATT_MAX_VALUE_SIZE	512
#define BT_GATT_MAX_SERVICES	32
#define BT_GATT_MAX_CHARS		64

struct bt_le_gatt_mtu_params {
	uint16		conn_handle;
	uint16		desired_mtu;
	// output
	status_t	status;
	uint16		negotiated_mtu;
};

struct bt_le_gatt_discover_services_params {
	uint16			conn_handle;
	// output
	status_t		status;
	uint16			count;
	gatt_service_t	services[BT_GATT_MAX_SERVICES];
};

struct bt_le_gatt_discover_chars_params {
	uint16					conn_handle;
	uint16					start_handle;
	uint16					end_handle;
	// output
	status_t				status;
	uint16					count;
	gatt_characteristic_t	chars[BT_GATT_MAX_CHARS];
};

struct bt_le_gatt_discover_descriptors_params {
	uint16					conn_handle;
	uint16					start_handle;
	uint16					end_handle;
	// inout
	gatt_characteristic_t	characteristic;
	status_t				status;
};

struct bt_le_gatt_read_params {
	uint16		conn_handle;
	uint16		attr_handle;
	// output
	status_t	status;
	uint16		value_length;
	uint8		value[BT_GATT_MAX_VALUE_SIZE];
};

struct bt_le_gatt_write_params {
	uint16		conn_handle;
	uint16		attr_handle;
	uint16		value_length;
	uint8		value[BT_GATT_MAX_VALUE_SIZE];
	// output
	status_t	status;
};

struct bt_le_gatt_subscribe_params {
	uint16		conn_handle;
	uint16		ccc_handle;
	bool		enable;
	// output
	status_t	status;
};

struct bt_le_smp_pair_params {
	uint16		conn_handle;
	uint32		passkey;
};

struct bt_le_nc_confirm_params {
	uint16		conn_handle;
	bool		confirmed;
};


#endif // _BTHCI_TRANSPORT_H_
