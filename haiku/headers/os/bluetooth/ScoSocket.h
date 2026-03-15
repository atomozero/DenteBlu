/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * ScoSocket — SCO (Synchronous Connection-Oriented) link for voice audio.
 * Sets up an eSCO/SCO connection to a remote device via HCI commands,
 * and provides Send/Receive for 8kHz/16kHz PCM audio.
 */
#ifndef _SCO_SOCKET_H
#define _SCO_SOCKET_H

#include <OS.h>
#include <SupportDefs.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/HCI/btHCI.h>


namespace Bluetooth {


class ScoSocket {
public:
							ScoSocket();
	virtual					~ScoSocket();

	/* Connect: sets up eSCO connection via HCI Setup Synchronous
	 * Connection command. Requires an existing ACL link to the device.
	 * aclHandle is the ACL connection handle. */
	status_t				Connect(const bdaddr_t& address,
								uint16 aclHandle);

	/* Accept: accepts an incoming SCO connection request from the
	 * given BD_ADDR. */
	status_t				Accept(const bdaddr_t& address);

	void					Disconnect();
	bool					IsConnected() const;

	/* SCO connection handle (after connect/accept) */
	uint16					Handle() const { return fScoHandle; }

	/* Audio parameters from the controller */
	uint16					RxPacketLength() const
								{ return fRxPktLen; }
	uint16					TxPacketLength() const
								{ return fTxPktLen; }
	uint8					LinkType() const { return fLinkType; }

private:
	static bool				_GetHciId(hci_id& outHid);

	bdaddr_t				fRemoteAddr;
	uint16					fScoHandle;
	uint16					fRxPktLen;
	uint16					fTxPktLen;
	uint8					fLinkType;
	bool					fConnected;
};


} /* namespace Bluetooth */


#endif /* _SCO_SOCKET_H */
