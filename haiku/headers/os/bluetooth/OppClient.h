/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * OppClient — Object Push Profile client for Classic Bluetooth.
 *
 * Sends files to a remote device via OBEX Push. Connects over RFCOMM
 * (querying SDP for UUID 0x1105 to find the OBEX Object Push channel).
 */
#ifndef _OPP_CLIENT_H_
#define _OPP_CLIENT_H_

#include <SupportDefs.h>

#include <bluetooth/bluetooth.h>


class ObexClient;
class RfcommSession;


namespace Bluetooth {


class OppClient {
public:
								OppClient();
	virtual						~OppClient();

	/* Connect to a remote device's OPP service.
	 * If rfcommChannel == 0, queries SDP for UUID 0x1105. */
	status_t					Connect(const bdaddr_t& address,
									uint8 rfcommChannel = 0);
	void						Disconnect();
	bool						IsConnected() const;

	/* Push a file to the remote device.
	 * filePath: local path to file to send */
	status_t					PushFile(const char* filePath);

	/* Push raw data with a given name and MIME type. */
	status_t					PushData(const char* name,
									const char* mimeType,
									const uint8* data, size_t dataLen);

private:
	static bool					_EnsureAclConnection(
									const bdaddr_t& remote);
	static bool					_QuerySdpForOpp(
									const bdaddr_t& remote,
									uint8& outChannel);

	RfcommSession*				fRfcomm;
	ObexClient*					fObex;
	uint8						fDlci;
};


} /* namespace Bluetooth */


#endif /* _OPP_CLIENT_H_ */
