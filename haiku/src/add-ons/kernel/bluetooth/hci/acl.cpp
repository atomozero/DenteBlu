/*
 * Copyright 2008 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <KernelExport.h>
#include <string.h>

#include <NetBufferUtilities.h>
#include <net_protocol.h>

#include <bluetooth/HCI/btHCI_acl.h>
#include <bluetooth/HCI/btHCI_transport.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/bdaddrUtils.h>

#include <btDebug.h>
#include <btCoreData.h>
#include <btModules.h>
#include <l2cap.h>

#include "acl.h"
#include "AttChannel.h"
#include "SmpManager.h"

extern struct bluetooth_core_data_module_info* btCoreData;
extern net_buffer_module_info* gBufferModule;

struct net_protocol_module_info* L2cap = NULL;

extern void RegisterConnection(hci_id hid, uint16 handle);
extern void unRegisterConnection(hci_id hid, uint16 handle);

status_t PostToUpper(HciConnection* conn, net_buffer* buf);

status_t
AclAssembly(net_buffer* nbuf, hci_id hid)
{
	status_t	error = B_OK;

	// Check ACL data packet. Driver should ensure report complete ACL packets
	if (nbuf->size < sizeof(struct hci_acl_header)) {
		ERROR("%s: Invalid ACL data packet, small length=%" B_PRIu32 "\n",
			__func__, nbuf->size);
		gBufferModule->free(nbuf);
		return (EMSGSIZE);
	}

	// Strip ACL data packet header
	NetBufferHeaderReader<struct hci_acl_header> aclHeader(nbuf);
	status_t status = aclHeader.Status();
	if (status < B_OK) {
		gBufferModule->free(nbuf);
		return ENOBUFS;
	}

	// Get ACL connection handle, PB flag and payload length
	aclHeader->handle = B_LENDIAN_TO_HOST_INT16(aclHeader->handle);

	uint16 con_handle = get_acl_handle(aclHeader->handle);
	uint16 pb = get_acl_pb_flag(aclHeader->handle);
	uint16 length = B_LENDIAN_TO_HOST_INT16(aclHeader->alen);

	aclHeader.Remove();

	TRACE("%s: ACL data packet, handle=%#x, PB=%#x, length=%d\n", __func__,
		con_handle, pb, length);

	// a) Ensure there is HCI connection
	// b) Get connection descriptor
	// c) veryfy the status of the connection

	HciConnection* conn = btCoreData->ConnectionByHandle(con_handle, hid);
	if (conn == NULL) {
		ERROR("%s: expected handle=%#x does not exist!\n", __func__,
			con_handle);
		conn = btCoreData->AddConnection(con_handle, BT_ACL, BDADDR_NULL, hid);
	}

	// Verify connection state
	if (conn->status!= HCI_CONN_OPEN) {
		ERROR("%s: unexpected ACL data packet. Connection not open\n",
			__func__);
		gBufferModule->free(nbuf);
		return EHOSTDOWN;
	}


	// Process packet
	if (pb == HCI_ACL_PACKET_START) {
		if (conn->currentRxPacket != NULL) {
			TRACE("%s: Dropping incomplete L2CAP packet, got %" B_PRIu32
				" want %d \n", __func__, conn->currentRxPacket->size, length );
			gBufferModule->free(conn->currentRxPacket);
			conn->currentRxPacket = NULL;
			conn->currentRxExpectedLength = 0;
		}

		// Get L2CAP header, ACL header was dimissed
		if (nbuf->size < sizeof(l2cap_basic_header)) {
			TRACE("%s: Invalid L2CAP start fragment, small, length=%" B_PRIu32
				"\n", __func__, nbuf->size);
			gBufferModule->free(nbuf);
			return (EMSGSIZE);
		}

		NetBufferHeaderReader<l2cap_basic_header> l2capHeader(nbuf);
		if (l2capHeader.Status() != B_OK) {
			gBufferModule->free(nbuf);
			return ENOBUFS;
		}

		TRACE("%s: New L2CAP, handle=%#x length=%d\n", __func__, con_handle,
			le16toh(l2capHeader->length));

		// Start new L2CAP packet
		conn->currentRxPacket = nbuf;
		conn->currentRxExpectedLength = B_LENDIAN_TO_HOST_INT16(l2capHeader->length)
			+ sizeof(l2cap_basic_header);
	} else if (pb == HCI_ACL_PACKET_FRAGMENT) {
		if (conn->currentRxPacket == NULL) {
			gBufferModule->free(nbuf);
			return (EINVAL);
		}

		// Add fragment to the L2CAP packet
		gBufferModule->merge(conn->currentRxPacket, nbuf, true);
	} else {
		ERROR("%s: invalid ACL data packet. Invalid PB flag=%#x\n", __func__,
			pb);
		gBufferModule->free(nbuf);
		return (EINVAL);
	}

	// substract the length of content of the ACL packet
	conn->currentRxExpectedLength -= length;

	if (conn->currentRxExpectedLength < 0) {
		TRACE("%s: Mismatch. Got %" B_PRIu32 ", expected %" B_PRIuSIZE "\n",
			__func__, conn->currentRxPacket->size,
			conn->currentRxExpectedLength);

		gBufferModule->free(conn->currentRxPacket);
		conn->currentRxPacket = NULL;
		conn->currentRxExpectedLength = 0;

	} else if (conn->currentRxExpectedLength == 0) {
		// OK, we have got complete L2CAP packet, so process it
		TRACE("%s: L2cap packet ready %" B_PRIu32 " bytes\n", __func__,
			conn->currentRxPacket->size);

		memcpy(conn->currentRxPacket->source, &conn->address_dest, sizeof(sockaddr_storage));
		conn->currentRxPacket->interface_address = &conn->interface_address;

		error = PostToUpper(conn, conn->currentRxPacket);
		// clean
		conn->currentRxPacket = NULL;
		conn->currentRxExpectedLength = 0;
	} else {
		TRACE("%s: Expected %ld current adds %d\n", __func__,
			conn->currentRxExpectedLength, length);
	}

	return error;
}


status_t
PostToUpper(HciConnection* conn, net_buffer* buf)
{
	// For LE connections, check if this is an ATT packet (CID 0x0004)
	// and route it directly to the AttChannel
	if (conn->isLE && buf->size >= sizeof(l2cap_basic_header)) {
		l2cap_basic_header l2hdr;
		gBufferModule->read(buf, 0, &l2hdr, sizeof(l2hdr));
		uint16 cid = B_LENDIAN_TO_HOST_INT16(l2hdr.dcid);

		if (cid == L2CAP_ATT_CID && conn->attChannel != NULL) {
			// Strip L2CAP header, pass ATT payload to AttChannel
			gBufferModule->remove_header(buf, sizeof(l2cap_basic_header));
			return ((AttChannel*)conn->attChannel)->ReceiveData(buf);
		}

		if (cid == L2CAP_SMP_CID && conn->smpManager != NULL) {
			// Strip L2CAP header, pass SMP payload to SmpManager
			gBufferModule->remove_header(buf, sizeof(l2cap_basic_header));
			return ((SmpManager*)conn->smpManager)->ReceiveData(buf);
		}
	}

	if (L2cap == NULL)

	if (get_module(NET_BLUETOOTH_L2CAP_NAME, (module_info**)&L2cap) != B_OK) {
		ERROR("%s: cannot get module \"%s\"\n", __func__,
			NET_BLUETOOTH_L2CAP_NAME);
		return B_ERROR;
	} // TODO: someone put it

	return L2cap->receive_data(buf);
}
