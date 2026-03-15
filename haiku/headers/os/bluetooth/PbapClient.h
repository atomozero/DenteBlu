/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * PbapClient — Phone Book Access Profile client for Classic Bluetooth.
 *
 * Connects to a remote device's PBAP server (PSE) and pulls phonebook
 * data as vCard. Uses OBEX over L2CAP (GOEP 2.0) or RFCOMM.
 */
#ifndef _PBAP_CLIENT_H_
#define _PBAP_CLIENT_H_

#include <SupportDefs.h>

#include <bluetooth/bluetooth.h>


class ObexClient;
class RfcommSession;


namespace Bluetooth {


class PbapClient {
public:
								PbapClient();
	virtual						~PbapClient();

	/* Connect to a remote device's PBAP PSE service.
	 * If rfcommChannel == 0, queries SDP for UUID 0x112F.
	 * Prefers L2CAP (GOEP 2.0) if the PSE advertises GoepL2capPsm. */
	status_t					Connect(const bdaddr_t& address,
									uint8 rfcommChannel = 0);

	/* Force L2CAP GOEP 2.0 transport (requires ERTM).
	 * Queries SDP for GoepL2capPsm and connects via L2CAP only.
	 * Fails if the remote device doesn't advertise GoepL2capPsm. */
	status_t					ConnectL2cap(const bdaddr_t& address);

	void						Disconnect();
	bool						IsConnected() const;

	/* Pull entire phonebook as vCard data.
	 * path: phonebook path (e.g., "telecom/pb.vcf")
	 * format: PBAP_FORMAT_VCARD_21 or PBAP_FORMAT_VCARD_30
	 * outData: receives vCard data (caller must free())
	 * outLen: receives data length */
	status_t					PullPhoneBook(const char* path,
									uint8 format,
									uint8** outData, size_t* outLen);

	/* Get phonebook size (number of entries) without downloading data.
	 * Sends MaxListCount=0 which triggers a size-only response. */
	status_t					GetPhoneBookSize(const char* path,
									uint16* outSize);

	/* Navigate to a folder (OBEX SETPATH).
	 * name: folder name, or NULL to go to root. */
	status_t					SetPath(const char* name);

	/* Pull phonebook listing (vCard-listing XML).
	 * path: folder path (e.g., "telecom/pb")
	 * outData: receives listing data (caller must free())
	 * outLen: receives data length */
	status_t					PullvCardListing(const char* path,
									uint8** outData, size_t* outLen);

private:
	static bool					_EnsureAclConnection(
									const bdaddr_t& remote);
	static bool					_QuerySdpForPbap(
									const bdaddr_t& remote,
									uint8& outChannel,
									uint16& outL2capPsm);

	RfcommSession*				fRfcomm;
	ObexClient*					fObex;
	uint8						fDlci;
	int							fL2capSocket;
};


} /* namespace Bluetooth */


#endif /* _PBAP_CLIENT_H_ */
