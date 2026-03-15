/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "l2cap_le.h"

#include <ByteOrder.h>
#include <NetBufferUtilities.h>

#include <btCoreData.h>
#include <btDebug.h>
#include <l2cap.h>

#include "l2cap_signal.h"


extern net_buffer_module_info* gBufferModule;
extern bluetooth_core_data_module_info* btCoreData;
extern bt_hci_module_info* btDevices;


/*
 * ATT receive callback type, set by the ATT module on the HciConnection.
 * The attChannel field holds a pointer to the ATT state object, and
 * the callback dispatches to the actual implementation.
 */
typedef status_t (*att_receive_callback)(void* channel, net_buffer* buffer);
static att_receive_callback sAttReceiveCallback = NULL;

void
l2cap_le_set_att_callback(att_receive_callback callback)
{
	sAttReceiveCallback = callback;
}


status_t
att_receive_data(struct HciConnection* conn, net_buffer* buffer)
{
	TRACE("%s: ATT data received, size=%" B_PRIu32 " handle=%#x\n",
		__func__, buffer->size, conn->handle);

	if (conn->attChannel != NULL && sAttReceiveCallback != NULL)
		return sAttReceiveCallback(conn->attChannel, buffer);

	TRACE("%s: no ATT handler for connection, dropping\n", __func__);
	gBufferModule->free(buffer);
	return B_OK;
}


typedef status_t (*smp_receive_callback)(void* manager, net_buffer* buffer);
static smp_receive_callback sSmpReceiveCallback = NULL;

void
l2cap_le_set_smp_callback(smp_receive_callback callback)
{
	sSmpReceiveCallback = callback;
}


status_t
smp_receive_data(struct HciConnection* conn, net_buffer* buffer)
{
	TRACE("%s: SMP data received, size=%" B_PRIu32 " handle=%#x\n",
		__func__, buffer->size, conn->handle);

	if (conn->smpManager != NULL && sSmpReceiveCallback != NULL)
		return sSmpReceiveCallback(conn->smpManager, buffer);

	TRACE("%s: no SMP handler for connection, dropping\n", __func__);
	gBufferModule->free(buffer);
	return B_OK;
}


status_t
l2cap_handle_le_signaling_command(struct HciConnection* conn,
	net_buffer* buffer)
{
	TRACE("%s: LE signaling command, size=%" B_PRIu32 "\n",
		__func__, buffer->size);

	while (buffer->size != 0) {
		NetBufferHeaderReader<l2cap_command_header> commandHeader(buffer);
		if (commandHeader.Status() != B_OK)
			return ENOBUFS;

		const uint8 code = commandHeader->code;
		const uint8 ident = commandHeader->ident;
		const uint16 length = B_LENDIAN_TO_HOST_INT16(commandHeader->length);

		if (buffer->size < length) {
			ERROR("%s: invalid LE signaling command: code=%#x ident=%d "
				"length=%d buffer=%" B_PRIu32 "\n", __func__, code, ident,
				length, buffer->size);
			gBufferModule->free(buffer);
			return EMSGSIZE;
		}

		commandHeader.Remove();

		switch (code) {
			case L2CAP_CONN_PARAM_UPDATE_REQ:
			{
				NetBufferHeaderReader<l2cap_conn_param_update_req>
					req(buffer);
				if (req.Status() != B_OK)
					break;

				TRACE("%s: Conn Param Update Req: interval=[%d,%d] "
					"latency=%d timeout=%d\n", __func__,
					B_LENDIAN_TO_HOST_INT16(req->interval_min),
					B_LENDIAN_TO_HOST_INT16(req->interval_max),
					B_LENDIAN_TO_HOST_INT16(req->latency),
					B_LENDIAN_TO_HOST_INT16(req->timeout));

				// Accept the request
				net_buffer* reply = gBufferModule->create(
					sizeof(l2cap_command_header)
					+ sizeof(l2cap_conn_param_update_rsp)
					+ sizeof(l2cap_basic_header));
				if (reply != NULL) {
					l2cap_conn_param_update_rsp rsp;
					rsp.result = B_HOST_TO_LENDIAN_INT16(
						l2cap_conn_param_update_rsp::RESULT_ACCEPTED);

					NetBufferPrepend<l2cap_conn_param_update_rsp>
						rspData(reply);
					memcpy(rspData.operator->(), &rsp, sizeof(rsp));
					rspData.Sync();

					NetBufferPrepend<l2cap_command_header> cmdHdr(reply);
					cmdHdr->code = L2CAP_CONN_PARAM_UPDATE_RSP;
					cmdHdr->ident = ident;
					cmdHdr->length = B_HOST_TO_LENDIAN_INT16(
						sizeof(l2cap_conn_param_update_rsp));
					cmdHdr.Sync();

					NetBufferPrepend<l2cap_basic_header> basic(reply);
					basic->length = B_HOST_TO_LENDIAN_INT16(reply->size
						- sizeof(l2cap_basic_header));
					basic->dcid = B_HOST_TO_LENDIAN_INT16(L2CAP_LE_SIGNALING_CID);
					basic.Sync();

					reply->type = conn->handle;
					btDevices->PostACL(conn->Hid, reply);
				}
				break;
			}

			case L2CAP_CONN_PARAM_UPDATE_RSP:
			{
				NetBufferHeaderReader<l2cap_conn_param_update_rsp>
					rsp(buffer);
				if (rsp.Status() != B_OK)
					break;

				TRACE("%s: Conn Param Update Rsp: result=%d\n", __func__,
					B_LENDIAN_TO_HOST_INT16(rsp->result));
				break;
			}

			case L2CAP_COMMAND_REJECT_RSP:
			{
				TRACE("%s: LE Command Reject\n", __func__);
				break;
			}

			default:
				ERROR("%s: Unknown LE signaling code=%#x\n", __func__,
					code);
				send_l2cap_command_reject(conn, ident,
					l2cap_command_reject::REJECTED_NOT_UNDERSTOOD,
					0, 0, 0);
				break;
		}

		gBufferModule->remove_header(buffer, length);
	}

	gBufferModule->free(buffer);
	return B_OK;
}
