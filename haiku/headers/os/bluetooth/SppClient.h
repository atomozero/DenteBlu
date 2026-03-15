/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SppClient — Serial Port Profile for Classic Bluetooth.
 *
 * Provides a virtual serial port over RFCOMM. Can connect to a remote
 * device (client mode) or listen for incoming connections (server mode).
 * Provides Send/Receive for bidirectional data transfer.
 */
#ifndef _SPP_CLIENT_H_
#define _SPP_CLIENT_H_

#include <SupportDefs.h>

#include <bluetooth/bluetooth.h>


class RfcommSession;


namespace Bluetooth {


typedef void (*spp_data_callback)(const void* data, size_t length,
	void* cookie);


class SppClient {
public:
								SppClient();
	virtual						~SppClient();

	/* Client: connect to a remote device's SPP service.
	 * If rfcommChannel == 0, queries SDP for UUID 0x1101 to find it. */
	status_t					Connect(const bdaddr_t& address,
									uint8 rfcommChannel = 0);

	/* Server: listen on L2CAP PSM 3 and accept one RFCOMM connection.
	 * Blocks until a remote device connects or timeout expires. */
	status_t					Listen(bigtime_t timeout = 30000000LL);

	void						Disconnect();
	bool						IsConnected() const;

	/* Data transfer */
	ssize_t						Send(const void* data, size_t length);
	ssize_t						Receive(void* buffer, size_t maxLength,
									bigtime_t timeout = 5000000LL);

	/* Optional async callback for received data */
	void						SetDataCallback(spp_data_callback callback,
									void* cookie);

	/* Accessors */
	uint16						Mtu() const;

private:
	static bool					_EnsureAclConnection(
									const bdaddr_t& remote);
	static bool					_QuerySdpForChannel(
									const bdaddr_t& remote,
									uint8& outChannel);

	RfcommSession*				fSession;
	int							fServerSocket;
	uint8						fDlci;
	spp_data_callback			fCallback;
	void*						fCallbackCookie;
};


} /* namespace Bluetooth */


#endif /* _SPP_CLIENT_H_ */
