#ifndef _BLUETOOTH_SERVER_PRIVATE_H
#define _BLUETOOTH_SERVER_PRIVATE_H


#define BLUETOOTH_SIGNATURE "application/x-vnd.Haiku-bluetooth_server"
#define BLUETOOTH_APP_SIGNATURE "application/x-vnd.Haiku-BluetoothPrefs"

/* Kit Comunication */

// LocalDevice
#define BT_MSG_COUNT_LOCAL_DEVICES		'btCd'
#define BT_MSG_ACQUIRE_LOCAL_DEVICE     'btAd'
#define BT_MSG_HANDLE_SIMPLE_REQUEST    'btsR'
#define BT_MSG_ADD_DEVICE               'btDD'
#define BT_MSG_REMOVE_DEVICE            'btrD'
#define BT_MSG_GET_PROPERTY             'btgP'

// Discovery
#define BT_MSG_INQUIRY_STARTED    'IqSt'
#define BT_MSG_INQUIRY_COMPLETED  'IqCM'
#define BT_MSG_INQUIRY_TERMINATED 'IqTR'
#define BT_MSG_INQUIRY_ERROR      'IqER'
#define BT_MSG_INQUIRY_DEVICE     'IqDE'

// LE Scanning
#define BT_MSG_LE_SCAN_START      'LsSt'
#define BT_MSG_LE_SCAN_STOP       'LsSp'
#define BT_MSG_LE_SCAN_RESULT     'LsRs'

// LE Connection
#define BT_MSG_LE_CONNECT         'LeCn'
#define BT_MSG_LE_DISCONNECT      'LeDc'
#define BT_MSG_LE_CONN_COMPLETE   'LeCc'

// LE GATT
#define BT_MSG_LE_GATT_DISCOVER   'LeGd'
#define BT_MSG_LE_GATT_READ       'LeGr'
#define BT_MSG_LE_GATT_WRITE      'LeGw'
#define BT_MSG_LE_GATT_SUBSCRIBE  'LeGs'

// LE Pairing
#define BT_MSG_LE_PAIR            'LePr'
#define BT_MSG_LE_NC_CONFIRM      'LeNc'

// Paired device management
#define BT_MSG_GET_PAIRED_DEVICES  'btPd'
#define BT_MSG_SAVE_DEVICE_NAME    'btSn'
#define BT_MSG_SET_AUTO_RECONNECT  'btAR'

// LE Scan results
#define BT_MSG_LE_SCAN_RESULTS_GET 'LsRg'

#endif
