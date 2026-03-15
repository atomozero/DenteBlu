/*
 * Copyright 2008 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#include <KernelExport.h>
#include <string.h>

#include <net/if_dl.h>
#include <net_datalink.h>
#include <NetBufferUtilities.h>

#include <l2cap.h>
#include "L2capEndpoint.h"
#include "L2capEndpointManager.h"
#include "l2cap_internal.h"
#include "l2cap_signal.h"
#include "l2cap_command.h"

#include <btDebug.h>

#include <bluetooth/L2CAP/btL2CAP.h>
#include <bluetooth/HCI/btHCI_command.h>


struct l2cap_config_options {
	uint16 mtu;
	bool mtu_set;
	uint16 flush_timeout;
	bool flush_timeout_set;
	l2cap_qos qos;
	bool qos_set;
	l2cap_rfc rfc;
	bool rfc_set;
	int8 fcs_option;	/* -1 = not present, 0 = no FCS, 1 = CRC-16 */
	net_buffer* rejected;
};


// #pragma mark - incoming signals


static status_t
l2cap_handle_connection_req(HciConnection* conn, uint8 ident, net_buffer* buffer)
{
	NetBufferHeaderReader<l2cap_connection_req> command(buffer);
	if (command.Status() != B_OK)
		return ENOBUFS;

	const uint16 psm = le16toh(command->psm);
	const uint16 scid = le16toh(command->scid);

	ERROR("l2cap: connection request psm=%d scid=%d\n", psm, scid);

	L2capEndpoint* endpoint = gL2capEndpointManager.ForPSM(psm);
	if (endpoint == NULL) {
		// Refuse connection.
		ERROR("l2cap: no endpoint for psm %d, refusing\n", psm);
		send_l2cap_connection_rsp(conn, ident, 0, scid,
			l2cap_connection_rsp::RESULT_PSM_NOT_SUPPORTED, 0);
		return B_OK;
	}

	ERROR("l2cap: routing connection to endpoint for psm %d\n", psm);
	endpoint->_HandleConnectionReq(conn, ident, psm, scid);
	return B_OK;
}


static void
l2cap_handle_connection_rsp(L2capEndpoint* endpoint, uint8 ident, net_buffer* buffer,
	bool& releaseIdent)
{
	if (endpoint == NULL) {
		ERROR("l2cap: unexpected connection response: ident %d unknown\n", ident);
		return;
	}

	NetBufferHeaderReader<l2cap_connection_rsp> command(buffer);
	if (command.Status() != B_OK)
		return;

	l2cap_connection_rsp response;
	response.dcid = le16toh(command->dcid);
	response.scid = le16toh(command->scid);
	response.result = le16toh(command->result);
	response.status = le16toh(command->status);

	ERROR("l2cap: connection rsp dcid=%d scid=%d result=%d status=%d\n",
		response.dcid, response.scid, response.result, response.status);

	if (response.result == l2cap_connection_rsp::RESULT_PENDING) {
		// Remote needs more time. Do NOT release the ident — the same ident
		// will be reused for the final response.
		releaseIdent = false;
	}

	endpoint->_HandleConnectionRsp(ident, response);
}


static void
parse_configuration_options(net_buffer* buffer, size_t offset, uint16 length,
	l2cap_config_options& options)
{
	while (offset < length) {
		l2cap_configuration_option option;
		if (gBufferModule->read(buffer, offset, &option, sizeof(option)) != B_OK)
			break;

		l2cap_configuration_option_value value;
		if (gBufferModule->read(buffer, offset + sizeof(option), &value,
				min_c(sizeof(value), option.length)) != B_OK) {
			break;
		}

		const bool hint = (option.type & l2cap_configuration_option::OPTION_HINT_BIT);
		option.type &= ~l2cap_configuration_option::OPTION_HINT_BIT;

		switch (option.type) {
			case l2cap_configuration_option::OPTION_MTU:
				options.mtu = le16toh(value.mtu);
				options.mtu_set = true;
				break;

			case l2cap_configuration_option::OPTION_FLUSH_TIMEOUT:
				options.flush_timeout = le16toh(value.flush_timeout);
				options.flush_timeout_set = true;
				break;

			case l2cap_configuration_option::OPTION_QOS:
				options.qos.flags = value.qos.flags;
				options.qos.service_type = value.qos.service_type;
				options.qos.token_rate = le32toh(value.qos.token_rate);
				options.qos.token_bucket_size = le32toh(value.qos.token_bucket_size);
				options.qos.peak_bandwidth = le32toh(value.qos.peak_bandwidth);
				options.qos.access_latency = le32toh(value.qos.access_latency);
				options.qos.delay_variation = le32toh(value.qos.delay_variation);
				options.qos_set = true;
				break;

			case l2cap_configuration_option::OPTION_RFC:
				options.rfc.mode = value.rfc.mode;
				options.rfc.tx_window_size = value.rfc.tx_window_size;
				options.rfc.max_transmit = value.rfc.max_transmit;
				options.rfc.retransmission_timeout = le16toh(value.rfc.retransmission_timeout);
				options.rfc.monitor_timeout = le16toh(value.rfc.monitor_timeout);
				options.rfc.max_pdu_size = le16toh(value.rfc.max_pdu_size);
				options.rfc_set = true;
				break;

			case l2cap_configuration_option::OPTION_FCS:
				if (option.length >= 1)
					options.fcs_option = value.mtu & 0xFF;
				break;

			default: {
				if (hint)
					break;

				// Unknown option: we need to reject it.
				if (options.rejected == NULL)
					options.rejected = gBufferModule->create(128);
				gBufferModule->append_cloned(options.rejected, buffer, offset,
					sizeof(option) + option.length);
			}
		}

		offset += sizeof(option) + option.length;
	}
}


static status_t
l2cap_handle_configuration_req(HciConnection* conn, uint8 ident, net_buffer* buffer, uint16 length)
{
	NetBufferHeaderReader<l2cap_configuration_req> command(buffer);
	if (command.Status() != B_OK)
		return ENOBUFS;

	const uint16 dcid = le16toh(command->dcid);
	L2capEndpoint* endpoint = gL2capEndpointManager.ForChannel(dcid);
	if (endpoint == NULL) {
		ERROR("l2cap: unexpected configuration req: channel does not exist (cid=%d)\n", dcid);
		send_l2cap_command_reject(conn, ident,
			l2cap_command_reject::REJECTED_INVALID_CID, 0, 0, 0);
		return B_ERROR;
	}

	const uint16 flags = le16toh(command->flags);

	// Read options (if any).
	l2cap_config_options options = {};
	options.fcs_option = -1;
	parse_configuration_options(buffer, sizeof(l2cap_configuration_req), length, options);

	if (options.rejected != NULL) {
		// Reject without doing anything else. Use remote's CID (not ours) as scid.
		send_l2cap_configuration_rsp(conn, ident, endpoint->RemoteChannelID(), 0,
			l2cap_configuration_rsp::RESULT_UNKNOWN_OPTION, options.rejected);
		return B_OK;
	}

	// If remote requests Streaming mode (unsupported), negotiate down.
	// ERTM (mode 3) is accepted.
	if (options.rfc_set && options.rfc.mode != l2cap_rfc::MODE_BASIC
			&& options.rfc.mode != l2cap_rfc::MODE_ERTM) {
		ERROR("l2cap: remote requested unsupported mode %d, proposing Basic Mode\n",
			options.rfc.mode);

		net_buffer* counterOpt = gBufferModule->create(128);
		if (counterOpt != NULL) {
			l2cap_configuration_option optHdr;
			optHdr.type = l2cap_configuration_option::OPTION_RFC;
			optHdr.length = sizeof(l2cap_rfc);
			gBufferModule->append(counterOpt, &optHdr, sizeof(optHdr));

			l2cap_rfc basicRfc = {};
			basicRfc.mode = l2cap_rfc::MODE_BASIC;
			gBufferModule->append(counterOpt, &basicRfc, sizeof(basicRfc));

			send_l2cap_configuration_rsp(conn, ident,
				endpoint->RemoteChannelID(), 0,
				l2cap_configuration_rsp::RESULT_UNACCEPTABLE_PARAMS, counterOpt);
			return B_OK;
		}
	}

	if (options.rfc_set) {
		ERROR("l2cap: remote config: mode=%d txwin=%d maxtx=%d maxpdu=%d\n",
			options.rfc.mode, options.rfc.tx_window_size,
			options.rfc.max_transmit, options.rfc.max_pdu_size);
	}

	endpoint->_HandleConfigurationReq(ident, flags,
		options.mtu_set ? &options.mtu : NULL,
		options.flush_timeout_set ? &options.flush_timeout : NULL,
		options.qos_set ? &options.qos : NULL,
		options.rfc_set ? &options.rfc : NULL,
		options.fcs_option);
	return B_OK;
}


static status_t
l2cap_handle_configuration_rsp(HciConnection* conn, L2capEndpoint* endpoint,
	uint8 ident, net_buffer* buffer, uint16 length, bool& releaseIdent)
{
	if (endpoint == NULL) {
		// The endpoint seems to be gone.
		return B_ERROR;
	}

	NetBufferHeaderReader<l2cap_configuration_rsp> command(buffer);
	if (command.Status() != B_OK)
		return ENOBUFS;

	const uint16 scid = le16toh(command->scid);
	const uint16 flags = le16toh(command->flags);
	const uint16 result = le16toh(command->result);
	releaseIdent = (flags & L2CAP_CFG_FLAG_CONTINUATION) == 0;

	// Read options (if any).
	l2cap_config_options options = {};
	options.fcs_option = -1;
	parse_configuration_options(buffer, sizeof(l2cap_configuration_rsp), length, options);

	if (options.rejected != NULL) {
		// Reject without doing anything else.
		send_l2cap_command_reject(conn, ident,
			l2cap_command_reject::REJECTED_NOT_UNDERSTOOD, 0, 0, 0);
		return B_OK;
	}

	endpoint->_HandleConfigurationRsp(ident, scid, flags, result,
		options.mtu_set ? &options.mtu : NULL,
		options.flush_timeout_set ? &options.flush_timeout : NULL,
		options.qos_set ? &options.qos : NULL,
		options.rfc_set ? &options.rfc : NULL,
		options.fcs_option);
	return B_OK;
}


static status_t
l2cap_handle_disconnection_req(HciConnection* conn, uint8 ident, net_buffer* buffer)
{
	NetBufferHeaderReader<l2cap_disconnection_req> command(buffer);
	if (command.Status() != B_OK)
		return ENOBUFS;

	const uint16 dcid = le16toh(command->dcid);
	const uint16 scid = le16toh(command->scid);
	ERROR("l2cap: disconnection req: dcid=%d scid=%d\n", dcid, scid);

	L2capEndpoint* endpoint = gL2capEndpointManager.ForChannel(dcid);
	if (endpoint == NULL) {
		ERROR("l2cap: unexpected disconnection req: channel does not exist (cid=%d)\n", dcid);
		send_l2cap_command_reject(conn, ident,
			l2cap_command_reject::REJECTED_INVALID_CID, 0, 0, 0);
		return B_ERROR;
	}

	endpoint->_HandleDisconnectionReq(ident, scid);
	return B_OK;
}


static status_t
l2cap_handle_disconnection_rsp(L2capEndpoint* endpoint, uint8 ident, net_buffer* buffer)
{
	if (endpoint == NULL) {
		ERROR("l2cap: unexpected disconnection response: ident %d unknown\n", ident);
		return B_ERROR;
	}

	NetBufferHeaderReader<l2cap_disconnection_rsp> command(buffer);
	if (command.Status() != B_OK)
		return ENOBUFS;

	const uint16 dcid = le16toh(command->dcid);
	const uint16 scid = le16toh(command->scid);

	endpoint->_HandleDisconnectionRsp(ident, dcid, scid);
	return B_OK;
}


static status_t
l2cap_handle_echo_req(HciConnection *conn, uint8 ident, net_buffer* buffer, uint16 length)
{
	net_buffer* reply = gBufferModule->clone(buffer, false);
	if (reply == NULL)
		return ENOMEM;

	gBufferModule->trim(reply, length);
	return send_l2cap_command(conn, L2CAP_ECHO_RSP, ident, reply);
}


static status_t
l2cap_handle_echo_rsp(L2capEndpoint* endpoint, uint8 ident, net_buffer* buffer, uint16 length)
{
	// Not currently triggered by this module.
	return B_OK;
}


static status_t
l2cap_handle_info_req(HciConnection* conn, uint8 ident, net_buffer* buffer)
{
	NetBufferHeaderReader<l2cap_information_req> command(buffer);
	if (command.Status() != B_OK)
		return ENOBUFS;

	const uint16 type = le16toh(command->type);
	ERROR("l2cap: info request type=%d\n", type);

	net_buffer* reply = NULL;
	uint8 replyCode = 0;
	switch (type) {
		case l2cap_information_req::TYPE_CONNECTIONLESS_MTU:
			reply = make_l2cap_information_rsp(replyCode, type,
				l2cap_information_rsp::RESULT_SUCCESS, L2CAP_MTU_DEFAULT);
			break;

		case l2cap_information_req::TYPE_EXTENDED_FEATURES:
			// Match Linux BlueZ: ERTM + Streaming + FCS + FixedChan
			// (0x0008 | 0x0010 | 0x0020 | 0x0080 = 0x00B8)
			reply = make_l2cap_information_rsp(replyCode, type,
				l2cap_information_rsp::RESULT_SUCCESS, 0x00B8);
			break;

		case l2cap_information_req::TYPE_FIXED_CHANNELS:
			// Report only L2CAP signaling channel (CID 0x0001 = bit 1)
			reply = make_l2cap_information_rsp(replyCode, type,
				l2cap_information_rsp::RESULT_SUCCESS, 0x0002);
			break;

	    default:
			ERROR("l2cap: unhandled information request type %d\n", type);
			reply = make_l2cap_information_rsp(replyCode, type,
				l2cap_information_rsp::RESULT_NOT_SUPPORTED, 0);
			break;
	}

	return send_l2cap_command(conn, replyCode, ident, reply);
}


static status_t
l2cap_handle_info_rsp(L2capEndpoint* endpoint, uint8 ident, net_buffer* buffer)
{
	NetBufferHeaderReader<l2cap_information_rsp> command(buffer);
	if (command.Status() != B_OK)
		return ENOBUFS;

	uint16 type = le16toh(command->type);
	uint16 result = le16toh(command->result);

	ERROR("l2cap: info rsp: type=%d result=%d\n", type, result);

	if (result == l2cap_information_rsp::RESULT_SUCCESS
			&& type == l2cap_information_req::TYPE_EXTENDED_FEATURES) {
		uint32 features = 0;
		gBufferModule->read(buffer, sizeof(l2cap_information_rsp),
			&features, sizeof(features));
		features = le32toh(features);
		ERROR("l2cap: info rsp: extended features=0x%04" B_PRIx32 "\n", features);
		if (endpoint != NULL)
			endpoint->_HandleInfoRsp(features);
	} else {
		// Not supported or different type — notify endpoint anyway
		if (endpoint != NULL)
			endpoint->_HandleInfoRsp(0);
	}

	return B_OK;
}


static status_t
l2cap_handle_command_reject(L2capEndpoint* endpoint, uint8 ident, net_buffer* buffer)
{
	if (endpoint == NULL) {
		ERROR("l2cap: unexpected command rejected: ident %d unknown\n", ident);
		return B_ERROR;
	}

	NetBufferHeaderReader<l2cap_command_reject> command(buffer);
	if (command.Status() != B_OK)
		return ENOBUFS;

	const uint16 reason = le16toh(command->reason);
	TRACE("%s: reason=%d\n", __func__, command->reason);

	l2cap_command_reject_data data = {};
	gBufferModule->read(buffer, sizeof(l2cap_command_reject),
		&data, min_c(buffer->size, sizeof(l2cap_command_reject_data)));

	endpoint->_HandleCommandRejected(ident, reason, data);

	return B_OK;
}


// #pragma mark - outgoing signals


status_t
send_l2cap_command(HciConnection* conn, uint8 code, uint8 ident, net_buffer* command)
{
	// TODO: Command timeouts (probably in L2capEndpoint.)
	{
		NetBufferPrepend<l2cap_command_header> header(command);
		header->code = code;
		header->ident = ident;
		header->length = htole16(command->size - sizeof(l2cap_command_header));
	}
	uint16 cmdLen = command->size;
	{
		NetBufferPrepend<l2cap_basic_header> basicHeader(command);
		basicHeader->length = htole16(command->size - sizeof(l2cap_basic_header));
		basicHeader->dcid = htole16(L2CAP_SIGNALING_CID);
	}
	ERROR("l2cap: TX signaling code=0x%02x ident=%d len=%d\n", code, ident,
		cmdLen - (uint16)sizeof(l2cap_command_header));
	command->type = conn->handle;
	status_t status = btDevices->PostACL(conn->Hid, command);
	if (status != B_OK)
		gBufferModule->free(command);
	return status;
}


status_t
send_l2cap_command_reject(HciConnection* conn, uint8 ident, uint16 reason,
	uint16 mtu, uint16 scid, uint16 dcid)
{
	uint8 code = 0;
	net_buffer* buffer = make_l2cap_command_reject(code, reason, mtu, scid, dcid);
	if (buffer == NULL)
		return ENOMEM;

	return send_l2cap_command(conn, code, ident, buffer);
}


status_t
send_l2cap_configuration_req(HciConnection* conn, uint8 ident, uint16 dcid, uint16 flags,
	uint16* mtu, uint16* flush_timeout, l2cap_qos* flow,
	l2cap_rfc* rfc, int8 fcsOption)
{
	uint8 code = 0;
	net_buffer* buffer = make_l2cap_configuration_req(code, dcid, flags,
		mtu, flush_timeout, flow, rfc, fcsOption);
	if (buffer == NULL)
		return ENOMEM;

	return send_l2cap_command(conn, code, ident, buffer);
}


status_t
send_l2cap_connection_req(HciConnection* conn, uint8 ident, uint16 psm, uint16 scid)
{
	uint8 code = 0;
	net_buffer* buffer = make_l2cap_connection_req(code, psm, scid);
	if (buffer == NULL)
		return ENOMEM;

	return send_l2cap_command(conn, code, ident, buffer);
}


status_t
send_l2cap_connection_rsp(HciConnection* conn, uint8 ident, uint16 dcid, uint16 scid,
	uint16 result, uint16 status)
{
	uint8 code = 0;
	net_buffer* buffer = make_l2cap_connection_rsp(code, dcid, scid, result, status);
	if (buffer == NULL)
		return ENOMEM;

	return send_l2cap_command(conn, code, ident, buffer);
}


status_t
send_l2cap_configuration_rsp(HciConnection* conn, uint8 ident, uint16 scid, uint16 flags,
	uint16 result, net_buffer* opt)
{
	uint8 code = 0;
	net_buffer* buffer = make_l2cap_configuration_rsp(code, scid, flags, result, opt);
	if (buffer == NULL)
		return ENOMEM;

	return send_l2cap_command(conn, code, ident, buffer);
}


status_t
send_l2cap_disconnection_req(HciConnection* conn, uint8 ident, uint16 dcid, uint16 scid)
{
	uint8 code = 0;
	net_buffer* buffer = make_l2cap_disconnection_req(code, dcid, scid);
	if (buffer == NULL)
		return ENOMEM;

	return send_l2cap_command(conn, code, ident, buffer);
}


status_t
send_l2cap_disconnection_rsp(HciConnection* conn, uint8 ident, uint16 dcid, uint16 scid)
{
	uint8 code = 0;
	net_buffer* buffer = make_l2cap_disconnection_rsp(code, dcid, scid);
	if (buffer == NULL)
		return ENOMEM;

	return send_l2cap_command(conn, code, ident, buffer);
}


// #pragma mark - dispatcher


status_t
l2cap_handle_signaling_command(HciConnection* connection, net_buffer* buffer)
{
	TRACE("%s: signal size=%" B_PRIu32 "\n", __func__, buffer->size);

	// TODO: If there are multiple commands in a packet, we could accumulate the
	// responses into a single packet also...

	while (buffer->size != 0) {
		NetBufferHeaderReader<l2cap_command_header> commandHeader(buffer);
		if (commandHeader.Status() != B_OK)
			return ENOBUFS;

		const uint8 code = commandHeader->code;
		const uint8 ident = commandHeader->ident;
		const uint16 length = le16toh(commandHeader->length);

		ERROR("l2cap: signaling cmd code=0x%02x ident=%d len=%d\n",
			code, ident, length);

		L2capEndpoint* endpoint = NULL;
		bool releaseIdent = false;
		if (L2CAP_IS_SIGNAL_RSP(code)) {
			endpoint = static_cast<L2capEndpoint*>(
				btCoreData->lookup_command_ident(connection, ident));
			releaseIdent = true;
		}

		if (buffer->size < length) {
			ERROR("%s: invalid L2CAP signaling command packet, code=%#x, "
				"ident=%d, length=%d, buffer size=%" B_PRIu32 "\n", __func__,
				code, ident, length, buffer->size);
			return EMSGSIZE;
		}

		commandHeader.Remove();

		switch (code) {
			case L2CAP_COMMAND_REJECT_RSP:
				l2cap_handle_command_reject(endpoint, ident, buffer);
				break;

			case L2CAP_CONNECTION_REQ:
				l2cap_handle_connection_req(connection, ident, buffer);
				break;

			case L2CAP_CONNECTION_RSP:
				l2cap_handle_connection_rsp(endpoint, ident, buffer, releaseIdent);
				break;

			case L2CAP_CONFIGURATION_REQ:
				l2cap_handle_configuration_req(connection, ident,
					buffer, length);
				break;

			case L2CAP_CONFIGURATION_RSP:
				l2cap_handle_configuration_rsp(connection, endpoint, ident,
					buffer, length, releaseIdent);
				break;

			case L2CAP_DISCONNECTION_REQ:
				l2cap_handle_disconnection_req(connection, ident, buffer);
				break;

			case L2CAP_DISCONNECTION_RSP:
				l2cap_handle_disconnection_rsp(endpoint, ident, buffer);
				break;

			case L2CAP_ECHO_REQ:
				l2cap_handle_echo_req(connection, ident, buffer, length);
				break;

			case L2CAP_ECHO_RSP:
				l2cap_handle_echo_rsp(endpoint, ident, buffer, length);
				break;

			case L2CAP_INFORMATION_REQ:
				l2cap_handle_info_req(connection, ident, buffer);
				break;

			case L2CAP_INFORMATION_RSP:
				l2cap_handle_info_rsp(endpoint, ident, buffer);
				break;

			default:
				ERROR("l2cap: unknown L2CAP signaling command, "
					"code=%#x\n", code);
				send_l2cap_command_reject(connection, ident,
					l2cap_command_reject::REJECTED_NOT_UNDERSTOOD, 0, 0, 0);
				break;
		}

		// Release the ident unless it needs to be preserved (e.g. RESULT_PENDING).
		// NOTE: releaseIdent may have been cleared by the handler above.
		if (releaseIdent)
			btCoreData->free_command_ident(connection, ident);

		// Advance to the next command (if any.)
		gBufferModule->remove_header(buffer, length);
	}

	gBufferModule->free(buffer);
	return B_OK;
}
