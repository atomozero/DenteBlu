/*
 * Copyright 2008, Oliver Ruiz Dorantes. All rights reserved.
 * Copyright 2024, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "L2capEndpoint.h"

#include <stdio.h>
#include <string.h>

#include <KernelExport.h>
#include <NetBufferUtilities.h>

#include <btDebug.h>
#include "L2capEndpointManager.h"
#include "l2cap_address.h"
#include "l2cap_command.h"
#include "l2cap_signal.h"


static l2cap_qos sDefaultQOS = {
	.flags = 0x0,
	.service_type = 1,
	.token_rate = 0xffffffff, /* maximum */
	.token_bucket_size = 0xffffffff, /* maximum */
	.peak_bandwidth = 0x00000000, /* maximum */
	.access_latency = 0xffffffff, /* don't care */
	.delay_variation = 0xffffffff /* don't care */
};


static inline bigtime_t
absolute_timeout(bigtime_t timeout)
{
	if (timeout == 0 || timeout == B_INFINITE_TIMEOUT)
		return timeout;

	return timeout + system_time();
}


static inline status_t
posix_error(status_t error)
{
	if (error == B_TIMED_OUT)
		return B_WOULD_BLOCK;

	return error;
}


/* CRC-16 for L2CAP FCS (BT Core Spec v5.x, Vol 3, Part A, 3.3.5).
 * Polynomial: g(D) = D^16 + D^15 + D^2 + 1 (0x8005, reflected: 0xA001)
 * Initial value: 0x0000
 * Matches Linux kernel crc16() used by BlueZ. */
static uint16
l2cap_fcs16_init(uint16 fcs, const uint8* data, size_t length)
{
	while (length--) {
		fcs ^= *data++;
		for (int i = 0; i < 8; i++) {
			if (fcs & 1)
				fcs = (fcs >> 1) ^ 0xA001;
			else
				fcs >>= 1;
		}
	}
	return fcs;
}


static inline uint16
l2cap_fcs16(const uint8* data, size_t length)
{
	return l2cap_fcs16_init(0x0000, data, length);
}


/* Compute FCS over a net_buffer's contents, continuing from a
 * running CRC state. */
static uint16
l2cap_fcs16_buffer_init(uint16 fcs, net_buffer* buffer, size_t offset,
	size_t length)
{
	uint8 chunk[64];
	while (length > 0) {
		size_t n = min_c(length, sizeof(chunk));
		if (gBufferModule->read(buffer, offset, chunk, n) != B_OK)
			break;
		fcs = l2cap_fcs16_init(fcs, chunk, n);
		offset += n;
		length -= n;
	}
	return fcs;
}


// #pragma mark - endpoint


L2capEndpoint::L2capEndpoint(net_socket* socket)
	:
	ProtocolSocket(socket),
	fAcceptSemaphore(-1),
	fState(CLOSED),
	fConnection(NULL),
	fChannelID(L2CAP_NULL_CID),
	fDestinationChannelID(L2CAP_NULL_CID),
	fPsm(0),
	fErtmMode(l2cap_rfc::MODE_BASIC),
	fTxSeq(0),
	fExpectedTxSeq(0),
	fReqSeq(0),
	fTxWindow(10),
	fMaxTransmit(10),
	fMaxPduSize(1024),
	fFcsEnabled(false),
	fRemoteExtendedFeatures(0),
	fUnackedCount(0),
	fExpectedAckSeq(0),
	fRemoteBusy(false),
	fRetransmitTimerActive(false),
	fMonitorTimerActive(false),
	fWaitF(false),
	fTimerSem(-1),
	fTimerThread(-1),
	fTimerFlags(0),
	fReassemblyBuffer(NULL),
	fReassemblyExpectedLen(0)
{
	CALLED();

	memset(fTxQueue, 0, sizeof(fTxQueue));
	memset(&fRetransmitTimer, 0, sizeof(fRetransmitTimer));
	memset(&fMonitorTimer, 0, sizeof(fMonitorTimer));

	mutex_init(&fLock, "l2cap endpoint");
	fCommandWait.Init(this, "l2cap endpoint command");

	// Set MTU and flow control settings to defaults
	fChannelConfig.incoming_mtu = L2CAP_MTU_DEFAULT;
	memcpy(&fChannelConfig.incoming_flow, &sDefaultQOS, sizeof(l2cap_qos));

	fChannelConfig.outgoing_mtu = L2CAP_MTU_DEFAULT;
	memcpy(&fChannelConfig.outgoing_flow, &sDefaultQOS, sizeof(l2cap_qos));

	fChannelConfig.flush_timeout = L2CAP_FLUSH_TIMEOUT_DEFAULT;
	fChannelConfig.link_timeout  = L2CAP_LINK_TIMEOUT_DEFAULT;

	fConfigState = {};

	gStackModule->init_fifo(&fReceiveQueue, "l2cap recv", L2CAP_MTU_MAXIMUM);
	gStackModule->init_fifo(&fSendQueue, "l2cap send", L2CAP_MTU_MAXIMUM);

	fTimerSem = create_sem(0, "l2cap ertm timer");
	fTimerThread = spawn_kernel_thread(_TimerThreadFunc,
		"l2cap ertm timer", B_NORMAL_PRIORITY, this);
	if (fTimerThread >= 0)
		resume_thread(fTimerThread);
}


L2capEndpoint::~L2capEndpoint()
{
	CALLED();

	// Stop ERTM timers and terminate the timer thread first.
	// This must happen before destroying the mutex, since the
	// timer thread acquires fLock.
	_StopRetransmitTimer();
	_StopMonitorTimer();
	if (fTimerSem >= 0) {
		delete_sem(fTimerSem);
		fTimerSem = -1;
	}
	if (fTimerThread >= 0) {
		status_t unused;
		wait_for_thread(fTimerThread, &unused);
		fTimerThread = -1;
	}

	mutex_lock(&fLock);
	mutex_destroy(&fLock);

	if (fState != CLOSED) {
		ERROR("L2capEndpoint: destroyed in state %d (expected CLOSED), forcing\n",
			fState);
		fState = CLOSED;
	}

	fCommandWait.NotifyAll(B_ERROR);

	// Free ERTM TX queue buffers
	for (int i = 0; i < kMaxTxWindow; i++) {
		if (fTxQueue[i].buffer != NULL) {
			gBufferModule->free(fTxQueue[i].buffer);
			fTxQueue[i].buffer = NULL;
		}
	}

	if (fReassemblyBuffer != NULL) {
		gBufferModule->free(fReassemblyBuffer);
		fReassemblyBuffer = NULL;
	}

	gStackModule->uninit_fifo(&fReceiveQueue);
	gStackModule->uninit_fifo(&fSendQueue);
}


status_t
L2capEndpoint::_WaitForStateChange(bigtime_t absoluteTimeout)
{
	channel_status state = fState;
	while (fState == state) {
		status_t status = fCommandWait.Wait(&fLock,
			B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT, absoluteTimeout);
		if (status != B_OK)
			return posix_error(status);
	}

	return B_OK;
}


status_t
L2capEndpoint::Open()
{
	CALLED();
	return ProtocolSocket::Open();
}


status_t
L2capEndpoint::Shutdown()
{
	CALLED();
	MutexLocker locker(fLock);

	if (fState == CLOSED) {
		// Nothing to do.
		return B_OK;
	}
	if (fState == LISTEN) {
		delete_sem(fAcceptSemaphore);
		fAcceptSemaphore = -1;
		gSocketModule->set_max_backlog(socket, 0);
		fState = BOUND;
		// fall through to unbind the PSM
	}
	if (fState == BOUND) {
		// Release the PSM binding now. Bind() called acquire_socket(),
		// adding an extra reference that would prevent Free() from ever
		// being called if we don't release it here.
		if (Domain() != NULL && !LocalAddress().IsEmpty(true))
			gL2capEndpointManager.Unbind(this);
		fState = CLOSED;
		return B_OK;
	}

	// Handle establishing states: connection or configuration not yet complete.
	// Free any pending command idents that point to this endpoint to prevent
	// dangling pointers if the remote sends a response after we're freed.
	if (_IsEstablishing()) {
		HciConnection* conn = fConnection;
		fConnection = NULL;
		_MarkClosed();
		// Drop fLock before calling btCoreData (which needs conn->fLock)
		// to maintain consistent lock ordering and avoid deadlock with
		// Disconnected() which acquires fChannelEndpointsLock then fLock.
		locker.Unlock();
		if (conn != NULL)
			btCoreData->free_command_idents_by_pointer(conn, this);
		return B_OK;
	}

	status_t status;
	bigtime_t timeout = absolute_timeout(socket->receive.timeout);
	if (gStackModule->is_restarted_syscall())
		timeout = gStackModule->restore_syscall_restart_timeout();
	else
		gStackModule->store_syscall_restart_timeout(timeout);

	while (fState > OPEN) {
		status = _WaitForStateChange(timeout);
		if (status != B_OK)
			return status;
	}
	if (fState == CLOSED)
		return B_OK;

	uint8 ident = btCoreData->allocate_command_ident(fConnection, this);
	if (ident == L2CAP_NULL_IDENT)
		return ENOBUFS;

	status = send_l2cap_disconnection_req(fConnection, ident,
		fDestinationChannelID, fChannelID);
	if (status != B_OK)
		return status;

	fState = WAIT_FOR_DISCONNECTION_RSP;

	while (fState != CLOSED) {
		status = _WaitForStateChange(timeout);
		if (status != B_OK)
			return status;
	}

	return B_OK;
}


status_t
L2capEndpoint::Close()
{
	return Shutdown();
}


status_t
L2capEndpoint::Free()
{
	CALLED();

	// Grab fConnection BEFORE removing from channel/PSM maps.
	// If we unbind first, a concurrent ~HciConnection() calling
	// Disconnected() won't find us in the channel map, so it can't
	// clear fConnection — leaving a dangling pointer that causes a
	// spinlock panic when free_command_idents_by_pointer tries to
	// lock the already-destroyed conn->fLock.
	// By reading fConnection first while we're still in the map,
	// Disconnected() can still race in and set fConnection = NULL
	// before we read it — safely preventing the dangling access.
	HciConnection* conn = NULL;
	{
		MutexLocker _(fLock);

		// If Shutdown() was never called (e.g. SIGKILL), run shutdown-like
		// cleanup to release resources that would otherwise leak.
		if (fState == LISTEN) {
			delete_sem(fAcceptSemaphore);
			fAcceptSemaphore = -1;
			fState = BOUND;
		}

		conn = fConnection;
		fConnection = NULL;
		fState = CLOSED;
	}

	// Now remove from PSM and channel maps. fLock is released, so
	// no lock ordering issue with fChannelEndpointsLock.
	if (Domain() != NULL && !LocalAddress().IsEmpty(true))
		gL2capEndpointManager.Unbind(this);

	if (fChannelID != L2CAP_NULL_CID)
		gL2capEndpointManager.UnbindFromChannel(this);

	// Safety net: purge any command idents that still reference this
	// endpoint. This prevents use-after-free if a response arrives
	// after the endpoint is destroyed.
	// free_command_idents_by_pointer validates that conn is still in
	// the active connection list under sConnectionListLock before
	// accessing conn->fLock — safe against concurrent deletion.
	if (conn != NULL)
		btCoreData->free_command_idents_by_pointer(conn, this);

	return B_OK;
}


status_t
L2capEndpoint::Bind(const struct sockaddr* _address)
{
	const sockaddr_l2cap* address
		= reinterpret_cast<const sockaddr_l2cap*>(_address);
	if (AddressModule()->is_empty_address(_address, true))
		return B_OK; // We don't need to bind to empty.
	if (!AddressModule()->is_same_family(_address))
		return EAFNOSUPPORT;
	if (address->l2cap_len != sizeof(struct sockaddr_l2cap))
		return EAFNOSUPPORT;

	CALLED();
	MutexLocker _(fLock);

	if (fState != CLOSED)
		return EISCONN;

	status_t status = gL2capEndpointManager.Bind(this, *address);
	if (status != B_OK)
		return status;

	fState = BOUND;
	return B_OK;
}


status_t
L2capEndpoint::Unbind()
{
	CALLED();
	MutexLocker _(fLock);

	if (LocalAddress().IsEmpty(true))
		return EINVAL;

	status_t status = gL2capEndpointManager.Unbind(this);
	if (status != B_OK)
		return status;

	if (fState == BOUND)
		fState = CLOSED;
	return B_OK;
}


status_t
L2capEndpoint::Listen(int backlog)
{
	CALLED();
	MutexLocker _(fLock);

	if (fState != BOUND)
		return B_BAD_VALUE;

	fAcceptSemaphore = create_sem(0, "l2cap accept");
	if (fAcceptSemaphore < B_OK) {
		ERROR("%s: Semaphore could not be created\n", __func__);
		return ENOBUFS;
	}

	gSocketModule->set_max_backlog(socket, backlog);

	fState = LISTEN;
	return B_OK;
}


status_t
L2capEndpoint::Connect(const struct sockaddr* _address)
{
	const sockaddr_l2cap* address
		= reinterpret_cast<const sockaddr_l2cap*>(_address);
	if (!AddressModule()->is_same_family(_address))
		return EAFNOSUPPORT;
	if (address->l2cap_len != sizeof(struct sockaddr_l2cap))
		return EAFNOSUPPORT;

	TRACE("l2cap: connect(\"%s\")\n",
		ConstSocketAddress(&gL2capAddressModule, _address).AsString().Data());
	MutexLocker _(fLock);

	status_t status;
	bigtime_t timeout = absolute_timeout(socket->send.timeout);
	if (gStackModule->is_restarted_syscall()) {
		timeout = gStackModule->restore_syscall_restart_timeout();

		while (fState != CLOSED && fState != OPEN) {
			status = _WaitForStateChange(timeout);
			if (status != B_OK)
				return status;
		}
		return (fState == OPEN) ? B_OK : ECONNREFUSED;
	} else {
		gStackModule->store_syscall_restart_timeout(timeout);
	}

	if (fState == LISTEN)
		return EINVAL;
	if (fState == OPEN)
		return EISCONN;
	if (fState != CLOSED)
		return EALREADY;

	// Set up route.
	hci_id hid = btCoreData->RouteConnection(address->l2cap_bdaddr);
	if (hid <= 0)
		return ENETUNREACH;

	TRACE("l2cap: %" B_PRId32 " for route %02x:%02x:%02x:%02x:%02x:%02x\n",
		hid, address->l2cap_bdaddr.b[5], address->l2cap_bdaddr.b[4],
		address->l2cap_bdaddr.b[3], address->l2cap_bdaddr.b[2],
		address->l2cap_bdaddr.b[1], address->l2cap_bdaddr.b[0]);

	fConnection = btCoreData->ConnectionByDestination(
		address->l2cap_bdaddr, hid);
	if (fConnection == NULL)
		return EHOSTUNREACH;

	memcpy(&socket->peer, _address, sizeof(struct sockaddr_l2cap));

	// Store PSM for ERTM decision in _SendChannelConfig.
	fPsm = address->l2cap_psm;

	status = gL2capEndpointManager.BindToChannel(this);
	if (status != B_OK)
		return status;

	fConfigState = {};

	// Send Info Request for Extended Features before Connection Request
	// (like BlueZ does to discover remote ERTM support).
	// Only for dynamic PSMs (>= 0x1000) that may use ERTM; SDP (PSM 1)
	// and RFCOMM (PSM 3) use Basic Mode and some phones get confused
	// by an Info Request before those connections.
	if (fPsm >= 0x1000) {
		uint8 infoIdent = btCoreData->allocate_command_ident(fConnection, this);
		if (infoIdent != L2CAP_NULL_IDENT) {
			uint8 infoCode = 0;
			net_buffer* infoReq = make_l2cap_information_req(infoCode,
				l2cap_information_req::TYPE_EXTENDED_FEATURES);
			if (infoReq != NULL) {
				status = send_l2cap_command(fConnection, infoCode, infoIdent, infoReq);
				if (status == B_OK) {
					fState = WAIT_FOR_INFO_RSP;
					bigtime_t infoTimeout = system_time() + 2000000;
					while (fState == WAIT_FOR_INFO_RSP) {
						status = _WaitForStateChange(infoTimeout);
						if (status != B_OK)
							break;
					}
					ERROR("l2cap: info rsp: remote features=0x%04" B_PRIx32 "\n",
						fRemoteExtendedFeatures);
					fState = CLOSED;
				}
			}
		}
	}

	uint8 ident = btCoreData->allocate_command_ident(fConnection, this);
	if (ident == L2CAP_NULL_IDENT)
		return ENOBUFS;

	status = send_l2cap_connection_req(fConnection, ident,
		address->l2cap_psm, fChannelID);
	if (status != B_OK)
		return status;

	fState = WAIT_FOR_CONNECTION_RSP;

	while (fState != CLOSED && fState != OPEN) {
		status = _WaitForStateChange(timeout);
		if (status != B_OK)
			return status;
	}
	return (fState == OPEN) ? B_OK : ECONNREFUSED;
}


status_t
L2capEndpoint::Accept(net_socket** _acceptedSocket)
{
	CALLED();
	MutexLocker locker(fLock);

	status_t status;
	bigtime_t timeout = absolute_timeout(socket->receive.timeout);
	if (gStackModule->is_restarted_syscall())
		timeout = gStackModule->restore_syscall_restart_timeout();
	else
		gStackModule->store_syscall_restart_timeout(timeout);

	do {
		locker.Unlock();

		status = acquire_sem_etc(fAcceptSemaphore, 1, B_ABSOLUTE_TIMEOUT
			| B_CAN_INTERRUPT, timeout);
		if (status != B_OK) {
			if (status == B_TIMED_OUT && socket->receive.timeout == 0)
				return B_WOULD_BLOCK;

			return status;
		}

		locker.Lock();
		status = gSocketModule->dequeue_connected(socket, _acceptedSocket);
	} while (status != B_OK);

	return status;
}


ssize_t
L2capEndpoint::ReadData(size_t numBytes, uint32 flags, net_buffer** _buffer)
{
	CALLED();

	*_buffer = NULL;

	bigtime_t timeout = 0;
	if ((flags & MSG_DONTWAIT) == 0) {
		timeout = absolute_timeout(socket->receive.timeout);
		if (gStackModule->is_restarted_syscall())
			timeout = gStackModule->restore_syscall_restart_timeout();
		else
			gStackModule->store_syscall_restart_timeout(timeout);
	}

	MutexLocker locker(fLock);
	ERROR("l2cap: ReadData: cid=%d state=%d fifo_bytes=%zu"
		" flags=0x%x timeout=%" B_PRIdBIGTIME "\n",
		fChannelID, fState, fReceiveQueue.current_bytes, flags, timeout);
	if (fState == CLOSED)
		flags |= MSG_DONTWAIT;
	locker.Unlock();

	// fLock must not be held during fifo_dequeue_buffer(), because
	// _HandleDisconnectionReq() needs fLock to mark the channel closed
	// and enqueue an EOF marker. Holding fLock here would deadlock.
	ssize_t result = gStackModule->fifo_dequeue_buffer(&fReceiveQueue,
		flags, timeout, _buffer);

	ERROR("l2cap: ReadData: dequeue result=%" B_PRIdSSIZE " buf=%p size=%"
		B_PRIu32 "\n", result, *_buffer,
		(*_buffer != NULL) ? (*_buffer)->size : 0);

	// _MarkClosed() enqueues a zero-length EOF marker. Convert it to a
	// NULL buffer so socket_receive() returns 0 (EOF) to userspace.
	if (result == B_OK && *_buffer != NULL && (*_buffer)->size == 0) {
		gBufferModule->free(*_buffer);
		*_buffer = NULL;
		ERROR("l2cap: ReadData: EOF marker dequeued\n");
		return B_OK;
	}

	if (result == B_WOULD_BLOCK) {
		// No data available. If the channel was closed while we were
		// waiting, return 0 (EOF) instead of an error.
		locker.Lock();
		if (fState == CLOSED) {
			ERROR("l2cap: ReadData: WOULD_BLOCK but CLOSED → EOF\n");
			return B_OK;
		}
	}

	return result;
}


status_t
L2capEndpoint::SendData(net_buffer* buffer)
{
	CALLED();
	MutexLocker locker(fLock);

	if (buffer == NULL)
		return ENOBUFS;

	if (fState != OPEN)
		return ENOTCONN;

	if (fErtmMode == l2cap_rfc::MODE_ERTM) {
		// In ERTM mode, send as I-frame(s). For simplicity we send
		// the entire buffer as a single unsegmented I-frame. If it
		// exceeds max PDU size, we split into SAR segments.
		bool consumed = false;
		while (buffer != NULL) {
			net_buffer* current = buffer;
			buffer = NULL;
			if (current->size > fMaxPduSize) {
				buffer = gBufferModule->split(current, fMaxPduSize);
				if (buffer == NULL) {
					if (consumed) {
						gBufferModule->free(current);
						break;
					}
					return ENOMEM;
				}
			}

			_SendErtmIFrame(current);
			consumed = true;
		}
		return B_OK;
	}

	// Basic Mode: existing logic
	// send_data protocol: return B_OK when buffer is consumed (ownership
	// transferred). On error, return a negative/POSIX error and do NOT free
	// the buffer — the network stack will free it. If we already consumed
	// some split chunks, we must return B_OK since those chunks are already
	// in the USB DMA pipeline and cannot be reclaimed.
	bool consumed = false;
	while (buffer != NULL) {
		net_buffer* current = buffer;
		buffer = NULL;
		if (current->size > fChannelConfig.outgoing_mtu) {
			// Break up into MTU-sized chunks.
			buffer = gBufferModule->split(current, fChannelConfig.outgoing_mtu);
			if (buffer == NULL) {
				if (consumed) {
					// Already sent prior chunks; free this remainder.
					gBufferModule->free(current);
					break;
				}
				return ENOMEM;
			}
		}

		status_t status = gStackModule->fifo_enqueue_buffer(&fSendQueue, current);
		if (status != B_OK) {
			if (consumed) {
				// Already sent prior chunks; free this failed chunk
				// and any remaining tail from a prior split.
				gBufferModule->free(current);
				if (buffer != NULL)
					gBufferModule->free(buffer);
				break;
			}
			// Nothing consumed yet. Do NOT free current — the network
			// stack owns it and will free on error return.
			return status;
		}

		consumed = true;
	}

	_SendQueued();

	return B_OK;
}


status_t
L2capEndpoint::ReceiveData(net_buffer* buffer)
{
	if (fErtmMode == l2cap_rfc::MODE_ERTM)
		return _HandleErtmFrame(buffer);

	// Basic Mode: FIXME: Check address specified in net_buffer!
	return gStackModule->fifo_enqueue_buffer(&fReceiveQueue, buffer);
}


// #pragma mark - ERTM frame handling


void
L2capEndpoint::_SendErtmIFrame(net_buffer* payload)
{
	ASSERT_LOCKED_MUTEX(&fLock);

	if (fState != OPEN || fConnection == NULL) {
		gBufferModule->free(payload);
		return;
	}

	uint32 payloadSize = payload->size;

	// Build I-frame control field
	uint16 control = 0;
	// bit 0 = 0 (I-frame)
	control |= ((uint16)(fTxSeq & 0x3F)) << L2CAP_CTRL_TXSEQ_SHIFT;
	control |= ((uint16)(fExpectedTxSeq & 0x3F)) << L2CAP_CTRL_REQSEQ_SHIFT;
	// SAR = unsegmented (bits 14-15 = 0)
	fReqSeq = fExpectedTxSeq;

	// Prepend control field
	uint16 ctrlLE = B_HOST_TO_LENDIAN_INT16(control);
	gBufferModule->prepend(payload, &ctrlLE, sizeof(ctrlLE));

	// Append FCS if negotiated.
	// Per BT spec 3.3.5: FCS covers the Basic L2CAP header (Length + CID),
	// Control field, and Information payload.
	if (fFcsEnabled) {
		// Build the L2CAP basic header bytes for FCS computation.
		// Length = current buffer size + 2 (FCS itself is included).
		uint8 hdr[4];
		uint16 lengthVal = payload->size + 2;
		hdr[0] = lengthVal & 0xFF;
		hdr[1] = (lengthVal >> 8) & 0xFF;
		hdr[2] = fDestinationChannelID & 0xFF;
		hdr[3] = (fDestinationChannelID >> 8) & 0xFF;

		// CRC over header, then continue over buffer (control + payload)
		uint16 fcs = l2cap_fcs16(hdr, 4);
		fcs = l2cap_fcs16_buffer_init(fcs, payload, 0, payload->size);
		ERROR("l2cap: ERTM I-frame FCS: input=%zu+%" B_PRIu32 "=%zu bytes, "
			"fcs=0x%04X\n", (size_t)4, payload->size,
			(size_t)4 + payload->size, fcs);
		uint16 fcsLE = B_HOST_TO_LENDIAN_INT16(fcs);
		gBufferModule->append(payload, &fcsLE, sizeof(fcsLE));
	}

	// Prepend L2CAP basic header
	NetBufferPrepend<l2cap_basic_header> header(payload);
	if (header.Status() != B_OK) {
		ERROR("l2cap: ERTM I-frame: header prepend failed\n");
		gBufferModule->free(payload);
		return;
	}
	header->length = B_HOST_TO_LENDIAN_INT16(payload->size - sizeof(l2cap_basic_header));
	header->dcid = B_HOST_TO_LENDIAN_INT16(fDestinationChannelID);

	ERROR("l2cap: ERTM TX I-frame: txseq=%d reqseq=%d payload=%" B_PRIu32
		" total=%" B_PRIu32 "\n",
		fTxSeq, fReqSeq, payloadSize, payload->size);

	// Hex dump of the complete frame for debugging
	{
		uint8 dump[128];
		size_t dumpLen = min_c(payload->size, sizeof(dump));
		if (gBufferModule->read(payload, 0, dump, dumpLen) == B_OK) {
			char hex[400];
			size_t pos = 0;
			for (size_t i = 0; i < dumpLen && pos < sizeof(hex) - 4; i++)
				pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", dump[i]);
			ERROR("l2cap: ERTM TX hex (%zu bytes): %s\n", dumpLen, hex);
		}
	}

	// Clone frame for retransmission buffer before sending
	net_buffer* clone = gBufferModule->clone(payload, false);
	if (clone != NULL) {
		uint8 idx = fTxSeq % kMaxTxWindow;
		if (fTxQueue[idx].buffer != NULL)
			gBufferModule->free(fTxQueue[idx].buffer);
		fTxQueue[idx].buffer = clone;
		fTxQueue[idx].txSeq = fTxSeq;
		fTxQueue[idx].retryCount = 0;
		fUnackedCount++;
	}

	fTxSeq = (fTxSeq + 1) & 0x3F;

	// Start retransmit timer
	_StartRetransmitTimer();

	payload->type = fConnection->handle;
	btDevices->PostACL(fConnection->ndevice->index, payload);
}


void
L2capEndpoint::_SendErtmSFrame(uint8 sType, bool poll, bool final)
{
	ASSERT_LOCKED_MUTEX(&fLock);

	if (fState != OPEN || fConnection == NULL)
		return;

	// Build S-frame control field
	uint16 control = 0x0001; // bit 0 = 1 (S-frame)
	control |= ((uint16)(sType & 0x03)) << L2CAP_CTRL_STYPE_SHIFT;
	control |= ((uint16)(fExpectedTxSeq & 0x3F)) << L2CAP_CTRL_REQSEQ_SHIFT;
	if (poll)
		control |= L2CAP_CTRL_P_BIT;
	if (final)
		control |= L2CAP_CTRL_F_BIT;

	fReqSeq = fExpectedTxSeq;

	// Create buffer with just the control field
	net_buffer* buffer = gBufferModule->create(16);
	if (buffer == NULL)
		return;

	uint16 ctrlLE = B_HOST_TO_LENDIAN_INT16(control);
	gBufferModule->append(buffer, &ctrlLE, sizeof(ctrlLE));

	// Append FCS if negotiated.
	// Per BT spec 3.3.5: FCS covers L2CAP header + Control.
	if (fFcsEnabled) {
		uint8 hdr[4];
		uint16 lengthVal = buffer->size + 2; // control(2) + FCS(2) = 4
		hdr[0] = lengthVal & 0xFF;
		hdr[1] = (lengthVal >> 8) & 0xFF;
		hdr[2] = fDestinationChannelID & 0xFF;
		hdr[3] = (fDestinationChannelID >> 8) & 0xFF;

		uint16 fcs = l2cap_fcs16(hdr, 4);
		fcs = l2cap_fcs16_init(fcs, (const uint8*)&ctrlLE, sizeof(ctrlLE));
		uint16 fcsLE = B_HOST_TO_LENDIAN_INT16(fcs);
		gBufferModule->append(buffer, &fcsLE, sizeof(fcsLE));
	}

	// Prepend L2CAP basic header
	NetBufferPrepend<l2cap_basic_header> header(buffer);
	if (header.Status() != B_OK) {
		gBufferModule->free(buffer);
		return;
	}
	header->length = B_HOST_TO_LENDIAN_INT16(buffer->size - sizeof(l2cap_basic_header));
	header->dcid = B_HOST_TO_LENDIAN_INT16(fDestinationChannelID);
	header.Sync();

	ERROR("l2cap: ERTM TX S-frame: type=%d reqseq=%d poll=%d final=%d"
		" size=%" B_PRIu32 "\n",
		sType, fExpectedTxSeq, poll, final, buffer->size);

	// Hex dump S-frame for debugging
	{
		uint8 dump[16];
		size_t dumpLen = min_c(buffer->size, sizeof(dump));
		if (gBufferModule->read(buffer, 0, dump, dumpLen) == B_OK) {
			char hex[64];
			size_t pos = 0;
			for (size_t i = 0; i < dumpLen && pos < sizeof(hex) - 4; i++)
				pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", dump[i]);
			ERROR("l2cap: ERTM TX S hex (%zu bytes): %s\n", dumpLen, hex);
		}
	}

	buffer->type = fConnection->handle;
	btDevices->PostACL(fConnection->ndevice->index, buffer);
}


status_t
L2capEndpoint::_HandleErtmFrame(net_buffer* buffer)
{
	MutexLocker locker(fLock);

	if (buffer->size < 2) {
		ERROR("l2cap: ERTM frame too short (%" B_PRIu32 " bytes)\n",
			buffer->size);
		gBufferModule->free(buffer);
		return B_ERROR;
	}

	// Read control field (first 2 bytes after basic header, which was
	// already stripped by l2cap_receive_data)
	uint16 ctrlLE;
	if (gBufferModule->read(buffer, 0, &ctrlLE, sizeof(ctrlLE)) != B_OK) {
		gBufferModule->free(buffer);
		return B_ERROR;
	}
	uint16 control = B_LENDIAN_TO_HOST_INT16(ctrlLE);

	// Validate FCS before processing (BT Core Spec Vol 3, Part A, 3.3.5)
	if (fFcsEnabled && buffer->size >= 4) {
		// FCS covers: L2CAP basic header (4 bytes, already stripped) +
		// remaining buffer content (control + payload + FCS).
		// The last 2 bytes of buffer are the received FCS.
		uint16 rxFcsLE;
		gBufferModule->read(buffer, buffer->size - 2, &rxFcsLE, 2);
		uint16 rxFcs = B_LENDIAN_TO_HOST_INT16(rxFcsLE);

		// Reconstruct L2CAP basic header for FCS computation
		uint8 hdr[4];
		uint16 lengthVal = buffer->size;
		hdr[0] = lengthVal & 0xFF;
		hdr[1] = (lengthVal >> 8) & 0xFF;
		hdr[2] = fChannelID & 0xFF;
		hdr[3] = (fChannelID >> 8) & 0xFF;

		// FCS over: header(4) + buffer content (excluding FCS bytes)
		uint16 computed = l2cap_fcs16(hdr, 4);
		computed = l2cap_fcs16_buffer_init(computed, buffer, 0,
			buffer->size - 2);

		if (computed != rxFcs) {
			ERROR("l2cap: ERTM FCS mismatch: computed=%04x received=%04x, "
				"dropping\n", computed, rxFcs);
			gBufferModule->free(buffer);
			return B_BAD_DATA;
		}

		// Strip validated FCS
		gBufferModule->trim(buffer, buffer->size - 2);
	}

	// Remove control field from buffer
	gBufferModule->remove_header(buffer, sizeof(ctrlLE));

	if (L2CAP_CTRL_IS_SFRAME(control)) {
		// S-frame: supervisory
		uint8 sType = (control & L2CAP_CTRL_STYPE_MASK) >> L2CAP_CTRL_STYPE_SHIFT;
		uint8 reqSeq = (control & L2CAP_CTRL_REQSEQ_MASK) >> L2CAP_CTRL_REQSEQ_SHIFT;
		bool poll = (control & L2CAP_CTRL_P_BIT) != 0;
		bool final = (control & L2CAP_CTRL_F_BIT) != 0;

		ERROR("l2cap: ERTM RX S-frame: type=%d reqseq=%d P=%d F=%d\n",
			sType, reqSeq, poll, final);

		if (sType == L2CAP_SFRAME_RR) {
			bool wasWaitingF = fWaitF;
			_ProcessAck(reqSeq);
			fRemoteBusy = false;
			if (final) {
				fWaitF = false;
				_StopMonitorTimer();
				// After RR+Poll / RR+Final exchange, retransmit
				// all unacked I-frames to recover from transient
				// packet loss (radio/USB). More aggressive than
				// BlueZ but necessary for reliable recovery.
				if (wasWaitingF && fUnackedCount > 0)
					_RetransmitFrom(fExpectedAckSeq);
			}
		} else if (sType == L2CAP_SFRAME_REJ) {
			_ProcessAck(reqSeq);
			_RetransmitFrom(reqSeq);
		} else if (sType == L2CAP_SFRAME_RNR) {
			_ProcessAck(reqSeq);
			fRemoteBusy = true;
		} else if (sType == L2CAP_SFRAME_SREJ) {
			_RetransmitSingle(reqSeq);
		}

		// If remote sent Poll, respond with RR+Final and retransmit
		// all unacked I-frames (BlueZ: l2cap_retransmit_all).
		if (poll) {
			if (fUnackedCount > 0)
				_RetransmitFrom(fExpectedAckSeq);
			_SendErtmSFrame(L2CAP_SFRAME_RR, false, true);
		}

		gBufferModule->free(buffer);
		return B_OK;
	}

	// I-frame
	uint8 txSeq = (control & L2CAP_CTRL_TXSEQ_MASK) >> L2CAP_CTRL_TXSEQ_SHIFT;
	uint8 reqSeq = (control & L2CAP_CTRL_REQSEQ_MASK) >> L2CAP_CTRL_REQSEQ_SHIFT;
	uint8 sar = (control & L2CAP_CTRL_SAR_MASK) >> L2CAP_CTRL_SAR_SHIFT;

	ERROR("l2cap: ERTM RX I-frame: txseq=%d reqseq=%d sar=%d len=%" B_PRIu32 "\n",
		txSeq, reqSeq, sar, buffer->size);

	// Process ACK from I-frame's ReqSeq
	_ProcessAck(reqSeq);

	// Check sequence number
	if (txSeq != fExpectedTxSeq) {
		ERROR("l2cap: ERTM seq mismatch: expected=%d got=%d\n",
			fExpectedTxSeq, txSeq);
		// Accept anyway for robustness (don't drop data)
	}
	fExpectedTxSeq = (txSeq + 1) & 0x3F;

	// Handle SAR reassembly
	_ErtmReassembleAndDeliver(buffer, sar);

	// Send RR acknowledgment
	_SendErtmSFrame(L2CAP_SFRAME_RR, false, false);

	return B_OK;
}


void
L2capEndpoint::_ErtmReassembleAndDeliver(net_buffer* payload, uint8 sar)
{
	ASSERT_LOCKED_MUTEX(&fLock);

	switch (sar) {
		case L2CAP_SAR_UNSEGMENTED: {
			// Complete SDU in one I-frame
			if (fReassemblyBuffer != NULL) {
				ERROR("l2cap: ERTM: unsegmented frame during reassembly, "
					"discarding incomplete SDU\n");
				gBufferModule->free(fReassemblyBuffer);
				fReassemblyBuffer = NULL;
			}
			ERROR("l2cap: ERTM: enqueuing %" B_PRIu32 " bytes to receive FIFO"
				" (fifo_bytes=%zu)\n",
				payload->size, fReceiveQueue.current_bytes);
			status_t enqStatus = gStackModule->fifo_enqueue_buffer(
				&fReceiveQueue, payload);
			ERROR("l2cap: ERTM: enqueue result=%s fifo_bytes=%zu\n",
				strerror(enqStatus), fReceiveQueue.current_bytes);
			break;
		}

		case L2CAP_SAR_START: {
			// First segment: payload starts with 2-byte SDU length
			if (fReassemblyBuffer != NULL) {
				ERROR("l2cap: ERTM: SAR_START during reassembly, "
					"discarding incomplete SDU\n");
				gBufferModule->free(fReassemblyBuffer);
			}

			// Read SDU length (first 2 bytes of payload)
			if (payload->size < 2) {
				ERROR("l2cap: ERTM: SAR_START too short\n");
				gBufferModule->free(payload);
				break;
			}
			uint16 sduLenLE;
			gBufferModule->read(payload, 0, &sduLenLE, sizeof(sduLenLE));
			fReassemblyExpectedLen = B_LENDIAN_TO_HOST_INT16(sduLenLE);

			// Remove SDU length field
			gBufferModule->remove_header(payload, 2);

			fReassemblyBuffer = payload;

			ERROR("l2cap: ERTM: SAR_START sdu_len=%d first_seg=%" B_PRIu32 "\n",
				fReassemblyExpectedLen, payload->size);
			break;
		}

		case L2CAP_SAR_CONTINUE:
			if (fReassemblyBuffer == NULL) {
				ERROR("l2cap: ERTM: SAR_CONTINUE without START, discarding\n");
				gBufferModule->free(payload);
				break;
			}
			gBufferModule->merge(fReassemblyBuffer, payload, true);
			ERROR("l2cap: ERTM: SAR_CONTINUE total=%" B_PRIu32 "/%d\n",
				fReassemblyBuffer->size, fReassemblyExpectedLen);
			break;

		case L2CAP_SAR_END:
			if (fReassemblyBuffer == NULL) {
				ERROR("l2cap: ERTM: SAR_END without START, discarding\n");
				gBufferModule->free(payload);
				break;
			}
			gBufferModule->merge(fReassemblyBuffer, payload, true);
			ERROR("l2cap: ERTM: SAR_END complete sdu=%" B_PRIu32 "/%d\n",
				fReassemblyBuffer->size, fReassemblyExpectedLen);
			gStackModule->fifo_enqueue_buffer(&fReceiveQueue,
				fReassemblyBuffer);
			fReassemblyBuffer = NULL;
			fReassemblyExpectedLen = 0;
			break;
	}
}


// #pragma mark - Basic Mode send


void
L2capEndpoint::_SendQueued()
{
	CALLED();
	ASSERT_LOCKED_MUTEX(&fLock);

	if (fState != OPEN || fConnection == NULL)
		return;

	net_buffer* buffer;
	while (gStackModule->fifo_dequeue_buffer(&fSendQueue, MSG_DONTWAIT, 0, &buffer) >= 0) {
		// Re-check connection after each dequeue in case it was torn down.
		if (fConnection == NULL) {
			gBufferModule->free(buffer);
			break;
		}

		uint32 payloadSize = buffer->size;

		NetBufferPrepend<l2cap_basic_header> header(buffer);
		if (header.Status() != B_OK) {
			ERROR("%s: header could not be prepended!\n", __func__);
			gBufferModule->free(buffer);
			continue;
		}

		header->length = B_HOST_TO_LENDIAN_INT16(buffer->size - sizeof(l2cap_basic_header));
		header->dcid = B_HOST_TO_LENDIAN_INT16(fDestinationChannelID);

		ERROR("l2cap: _SendQueued: cid=%d dcid=%d handle=%#x payload=%"
			B_PRIu32 " total=%" B_PRIu32 "\n",
			fChannelID, fDestinationChannelID,
			fConnection->handle, payloadSize, buffer->size);

		buffer->type = fConnection->handle;
		btDevices->PostACL(fConnection->ndevice->index, buffer);
	}
}


ssize_t
L2capEndpoint::Sendable()
{
	CALLED();
	MutexLocker locker(fLock);

	if (fState != OPEN) {
		if (_IsEstablishing())
			return 0;
		return EPIPE;
	}

	MutexLocker fifoLocker(fSendQueue.lock);
	return (fSendQueue.max_bytes - fSendQueue.current_bytes);
}


ssize_t
L2capEndpoint::Receivable()
{
	CALLED();
	MutexLocker locker(fLock);

	MutexLocker fifoLocker(fReceiveQueue.lock);
	return fReceiveQueue.current_bytes;
}


void
L2capEndpoint::_HandleCommandRejected(uint8 ident, uint16 reason,
	const l2cap_command_reject_data& data)
{
	CALLED();
	MutexLocker locker(fLock);

	switch (fState) {
		case WAIT_FOR_INFO_RSP:
			// Info request was rejected. Continue with connection.
			fState = CLOSED;
		break;

		case WAIT_FOR_CONNECTION_RSP:
			// Connection request was rejected. Reset state.
			fState = CLOSED;
			socket->error = ECONNREFUSED;
		break;

		case CONFIGURATION:
			// TODO: Adjust and resend configuration request.
		break;

	default:
		ERROR("l2cap: unknown command unexpectedly rejected (ident %d)\n", ident);
		break;
	}

	fCommandWait.NotifyAll();
}


void
L2capEndpoint::_HandleConnectionReq(HciConnection* connection,
	uint8 ident, uint16 psm, uint16 scid)
{
	MutexLocker locker(fLock);
	if (fState != LISTEN) {
		send_l2cap_connection_rsp(connection, ident, 0, scid,
			l2cap_connection_rsp::RESULT_PSM_NOT_SUPPORTED, 0);
		return;
	}
	locker.Unlock();

	net_socket* newSocket;
	status_t status = gSocketModule->spawn_pending_socket(socket, &newSocket);
	if (status != B_OK) {
		ERROR("l2cap: could not spawn child for endpoint: %s\n", strerror(status));
		send_l2cap_connection_rsp(connection, ident, 0, scid,
			l2cap_connection_rsp::RESULT_NO_RESOURCES, 0);
		return;
	}

	L2capEndpoint* endpoint = (L2capEndpoint*)newSocket->first_protocol;
	MutexLocker newEndpointLocker(endpoint->fLock);

	status = gL2capEndpointManager.BindToChannel(endpoint);
	if (status != B_OK) {
		ERROR("l2cap: could not allocate channel for endpoint: %s\n", strerror(status));
		send_l2cap_connection_rsp(connection, ident, 0, scid,
			l2cap_connection_rsp::RESULT_NO_RESOURCES, 0);
		return;
	}

	endpoint->fAcceptSemaphore = fAcceptSemaphore;

	endpoint->fConnection = connection;
	endpoint->fState = CONFIGURATION;
	endpoint->fPsm = psm;

	endpoint->fDestinationChannelID = scid;

	send_l2cap_connection_rsp(connection, ident, endpoint->fChannelID, scid,
		l2cap_connection_rsp::RESULT_SUCCESS, 0);

	ERROR("l2cap: incoming conn accepted: our_cid=%d remote_cid=%d psm=%d\n",
		endpoint->fChannelID, scid, psm);

	// Send our Configuration Request (both sides must exchange configs).
	endpoint->_SendChannelConfig();
}


void
L2capEndpoint::_HandleConnectionRsp(uint8 ident, const l2cap_connection_rsp& response)
{
	CALLED();
	MutexLocker locker(fLock);
	fCommandWait.NotifyAll();

	if (fState != WAIT_FOR_CONNECTION_RSP) {
		ERROR("l2cap: unexpected connection response, scid=%d, state=%d\n",
			response.scid, fState);
		send_l2cap_command_reject(fConnection, ident,
			l2cap_command_reject::REJECTED_INVALID_CID, 0, response.scid, response.dcid);
		return;
	}

	if (fChannelID != response.scid) {
		ERROR("l2cap: invalid connection response, mismatched SCIDs (%d, %d)\n",
			fChannelID, response.scid);
		send_l2cap_command_reject(fConnection, ident,
			l2cap_command_reject::REJECTED_INVALID_CID, 0, response.scid, response.dcid);
		return;
	}

	if (response.result == l2cap_connection_rsp::RESULT_PENDING) {
		// The connection is still pending on the remote end.
		// We will receive another CONNECTION_RSP later.

		// TODO: Increase/reset timeout? (We don't have any timeouts presently.)
		return;
	} else if (response.result != l2cap_connection_rsp::RESULT_SUCCESS) {
		// Some error response.
		// TODO: Translate `result` if possible?
		socket->error = ECONNREFUSED;

		fState = CLOSED;
		fCommandWait.NotifyAll();
		return;
	}

	// Success: channel is now open for configuration.
	fState = CONFIGURATION;
	fDestinationChannelID = response.dcid;

	_SendChannelConfig();
}


void
L2capEndpoint::_SendChannelConfig()
{
	uint16* mtu = NULL;
	uint16 mtuValue = fChannelConfig.incoming_mtu;

	// For dynamic PSMs (GOEP 2.0 etc.), request ERTM and larger MTU
	l2cap_rfc* rfc = NULL;
	l2cap_rfc ertmRfc = {};
	int8 fcsOption = -1;

	if (fPsm >= 0x1000) {
		// Dynamic PSM — request ERTM mode
		ertmRfc.mode = l2cap_rfc::MODE_ERTM;
		ertmRfc.tx_window_size = 10;
		ertmRfc.max_transmit = 10;
		ertmRfc.retransmission_timeout = 2000;	// BlueZ default (ms)
		ertmRfc.monitor_timeout = 12000;		// BlueZ default (ms)
		ertmRfc.max_pdu_size = 1024;
		rfc = &ertmRfc;

		// Don't include FCS option in Config Request — both sides
		// default to CRC-16 (BT Core Spec Vol 3, Part A, 5.5.3).
		// This avoids ambiguity with phones that don't include FCS
		// in their Config Request (moto g15 Android 14+ quirk).

		// Request larger MTU for data transfer
		mtuValue = 1024;
		mtu = &mtuValue;

		ERROR("l2cap: _SendChannelConfig: requesting ERTM for dynamic PSM 0x%04x\n",
			fPsm);
	} else if (fPsm == 3 /* RFCOMM */) {
		// Request larger MTU for RFCOMM so OBEX can send bigger
		// packets, reducing the number of round-trips for OPP.
		mtuValue = 4096;
		mtu = &mtuValue;
	} else {
		if (fChannelConfig.incoming_mtu != L2CAP_MTU_DEFAULT)
			mtu = &mtuValue;
	}

	uint16* flush_timeout = NULL;
	if (fChannelConfig.flush_timeout != L2CAP_FLUSH_TIMEOUT_DEFAULT)
		flush_timeout = &fChannelConfig.flush_timeout;

	l2cap_qos* flow = NULL;
	if (memcmp(&sDefaultQOS, &fChannelConfig.outgoing_flow,
			sizeof(fChannelConfig.outgoing_flow)) != 0) {
		flow = &fChannelConfig.outgoing_flow;
	}

	uint8 ident = btCoreData->allocate_command_ident(fConnection, this);
	if (ident == L2CAP_NULL_IDENT) {
		ERROR("l2cap: _SendChannelConfig: failed to allocate ident (cid=%d)\n",
			fChannelID);
		// TODO: Retry later?
		return;
	}

	status_t status = send_l2cap_configuration_req(fConnection, ident,
		fDestinationChannelID, 0, mtu, flush_timeout, flow, rfc, fcsOption);
	if (status != B_OK) {
		ERROR("l2cap: _SendChannelConfig: send failed (cid=%d, status=%s)\n",
			fChannelID, strerror(status));
		socket->error = status;
		return;
	}

	ERROR("l2cap: _SendChannelConfig: sent config req ident=%d for dcid=%d (our cid=%d)\n",
		ident, fDestinationChannelID, fChannelID);
	fConfigState.out = ConfigState::SENT;
}


void
L2capEndpoint::_HandleConfigurationReq(uint8 ident, uint16 flags,
	uint16* mtu, uint16* flush_timeout, l2cap_qos* flow,
	l2cap_rfc* rfc, int8 fcsOption)
{
	CALLED();
	MutexLocker locker(fLock);
	fCommandWait.NotifyAll();

	if (fState != CONFIGURATION && fState != OPEN) {
		ERROR("l2cap: unexpected configuration req: invalid channel state (cid=%d, state=%d)\n",
			fChannelID, fState);
		send_l2cap_configuration_rsp(fConnection, ident, fDestinationChannelID, 0,
			l2cap_configuration_rsp::RESULT_REJECTED, NULL);
		return;
	}

	if (fState == OPEN) {
		// Re-configuration.
		fConfigState = {};
		fState = CONFIGURATION;
	}

	// Process options.
	// TODO: Validate parameters!
	if (mtu != NULL && *mtu != fChannelConfig.outgoing_mtu) {
		ERROR("l2cap: remote MTU=%u (was %u), setting as outgoing_mtu\n",
			*mtu, fChannelConfig.outgoing_mtu);
		fChannelConfig.outgoing_mtu = *mtu;
	}
	if (flush_timeout != NULL && *flush_timeout != fChannelConfig.flush_timeout)
		fChannelConfig.flush_timeout = *flush_timeout;
	if (flow != NULL)
		fChannelConfig.incoming_flow = *flow;

	// Store ERTM parameters from remote's config request
	if (rfc != NULL) {
		if (rfc->mode == l2cap_rfc::MODE_ERTM) {
			fErtmMode = l2cap_rfc::MODE_ERTM;
			fTxWindow = rfc->tx_window_size;
			if (rfc->max_pdu_size > 0)
				fMaxPduSize = rfc->max_pdu_size;
			ERROR("l2cap: ERTM accepted from remote: txwin=%d maxpdu=%d\n",
				fTxWindow, fMaxPduSize);
		} else {
			fErtmMode = l2cap_rfc::MODE_BASIC;
		}
	}

	// FCS negotiation (BT Core Spec Vol 3, Part A, 5.5.3):
	// "FCS shall not be used if and only if both devices indicate 'No FCS'."
	// We always request No FCS in our Config Request for ERTM.
	// So FCS is disabled only if the remote ALSO explicitly requests No FCS.
	// If remote doesn't include FCS option, the default is FCS enabled.
	if (fcsOption == L2CAP_FCS_NONE)
		fFcsEnabled = false;
	else
		fFcsEnabled = true;

	ERROR("l2cap: _HandleConfigurationReq: cid=%d dcid=%d ident=%d configState.out=%d"
		" mode=%d fcs=%d\n",
		fChannelID, fDestinationChannelID, ident, fConfigState.out,
		fErtmMode, fFcsEnabled);

	// Build Config Response options: echo back MTU, RFC and FCS (like BlueZ).
	// We build the options in a stack byte array to avoid multi-append issues.
	net_buffer* rspOpt = NULL;
	if (rfc != NULL && rfc->mode == l2cap_rfc::MODE_ERTM) {
		// MTU(4) + RFC(11) + FCS(3) = 18 bytes max
		uint8 optBuf[18];
		size_t optLen = 0;

		// MTU option
		optBuf[optLen++] = l2cap_configuration_option::OPTION_MTU;
		optBuf[optLen++] = 2;
		uint16 mtuLE = B_HOST_TO_LENDIAN_INT16(fChannelConfig.outgoing_mtu);
		memcpy(optBuf + optLen, &mtuLE, 2);
		optLen += 2;

		// RFC option
		optBuf[optLen++] = l2cap_configuration_option::OPTION_RFC;
		optBuf[optLen++] = sizeof(l2cap_rfc);
		l2cap_rfc echoRfc;
		echoRfc.mode = rfc->mode;
		echoRfc.tx_window_size = rfc->tx_window_size;
		echoRfc.max_transmit = rfc->max_transmit;
		echoRfc.retransmission_timeout = B_HOST_TO_LENDIAN_INT16(
			rfc->retransmission_timeout ? rfc->retransmission_timeout : 2000);
		echoRfc.monitor_timeout = B_HOST_TO_LENDIAN_INT16(
			rfc->monitor_timeout ? rfc->monitor_timeout : 12000);
		echoRfc.max_pdu_size = B_HOST_TO_LENDIAN_INT16(rfc->max_pdu_size);
		memcpy(optBuf + optLen, &echoRfc, sizeof(echoRfc));
		optLen += sizeof(l2cap_rfc);

		// FCS option
		optBuf[optLen++] = l2cap_configuration_option::OPTION_FCS;
		optBuf[optLen++] = 1;
		optBuf[optLen++] = fFcsEnabled ? L2CAP_FCS_CRC16 : L2CAP_FCS_NONE;

		rspOpt = gBufferModule->create(256);
		if (rspOpt != NULL)
			gBufferModule->append(rspOpt, optBuf, optLen);
	}

	// Hex dump Config Response options for debugging
	if (rspOpt != NULL) {
		uint8 dump[32];
		size_t dumpLen = min_c(rspOpt->size, sizeof(dump));
		if (gBufferModule->read(rspOpt, 0, dump, dumpLen) == B_OK) {
			char hex[100];
			size_t pos = 0;
			for (size_t i = 0; i < dumpLen && pos < sizeof(hex) - 4; i++)
				pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", dump[i]);
			ERROR("l2cap: Config RSP options (%zu bytes): %s\n", dumpLen, hex);
		}
	}

	send_l2cap_configuration_rsp(fConnection, ident, fDestinationChannelID, 0,
		l2cap_configuration_rsp::RESULT_SUCCESS, rspOpt);

	if ((flags & L2CAP_CFG_FLAG_CONTINUATION) != 0) {
		// More options are coming, just keep waiting.
		return;
	}

	// We now have all options.
	fConfigState.in = ConfigState::DONE;

	if (fConfigState.out < ConfigState::SENT) {
		ERROR("l2cap: _HandleConfigurationReq: sending our config req now\n");
		_SendChannelConfig();
	} else if (fConfigState.out == ConfigState::DONE) {
		ERROR("l2cap: _HandleConfigurationReq: both done, marking established\n");
		_MarkEstablished();
	} else {
		ERROR("l2cap: _HandleConfigurationReq: waiting for config rsp (out=%d)\n",
			fConfigState.out);
	}
}


void
L2capEndpoint::_HandleConfigurationRsp(uint8 ident, uint16 scid, uint16 flags,
	uint16 result, uint16* mtu, uint16* flush_timeout,
	l2cap_qos* flow, l2cap_rfc* rfc, int8 fcsOption)
{
	CALLED();
	MutexLocker locker(fLock);
	fCommandWait.NotifyAll();

	if (fState != CONFIGURATION) {
		ERROR("l2cap: unexpected configuration rsp: invalid channel state (cid=%d, state=%d)\n",
			fChannelID, fState);
		send_l2cap_command_reject(fConnection, ident,
			l2cap_command_reject::REJECTED_INVALID_CID, 0, scid, fChannelID);
		return;
	}
	if (scid != fChannelID) {
		ERROR("l2cap: unexpected configuration rsp: invalid source channel (cid=%d, scid=%d)\n",
			fChannelID, scid);
		send_l2cap_command_reject(fConnection, ident,
			l2cap_command_reject::REJECTED_INVALID_CID, 0, scid, fChannelID);
		return;
	}

	// TODO: Validate parameters!
	if (result == l2cap_configuration_rsp::RESULT_PENDING) {
		// We will receive another CONFIGURATION_RSP later.
		return;
	} else if (result == l2cap_configuration_rsp::RESULT_UNACCEPTABLE_PARAMS) {
		// The acceptable parameters are specified in options.
		if (mtu != NULL && *mtu != fChannelConfig.incoming_mtu)
			fChannelConfig.incoming_mtu = *mtu;
		if (flush_timeout != NULL && *flush_timeout != fChannelConfig.flush_timeout)
			fChannelConfig.flush_timeout = *flush_timeout;

		// If remote rejected ERTM and proposed Basic Mode, accept
		if (rfc != NULL && rfc->mode == l2cap_rfc::MODE_BASIC) {
			ERROR("l2cap: remote rejected ERTM, falling back to Basic Mode\n");
			fErtmMode = l2cap_rfc::MODE_BASIC;
		} else if (rfc != NULL && rfc->mode == l2cap_rfc::MODE_ERTM) {
			// Remote accepted ERTM but wants different parameters
			fErtmMode = l2cap_rfc::MODE_ERTM;
			fTxWindow = rfc->tx_window_size;
			fMaxTransmit = rfc->max_transmit;
			if (rfc->max_pdu_size > 0)
				fMaxPduSize = rfc->max_pdu_size;
		}

		// FCS from response
		if (fcsOption == L2CAP_FCS_CRC16)
			fFcsEnabled = true;
		else if (fcsOption == L2CAP_FCS_NONE)
			fFcsEnabled = false;
	} else if (result == l2cap_configuration_rsp::RESULT_FLOW_SPEC_REJECTED) {
		if (flow != NULL)
			fChannelConfig.outgoing_flow = *flow;
	} else if (result != l2cap_configuration_rsp::RESULT_SUCCESS) {
		ERROR("l2cap: unhandled configuration response! (result=%d)\n",
			result);
		return;
	}

	if ((flags & L2CAP_CFG_FLAG_CONTINUATION) != 0) {
		// More options are coming, just keep waiting.
		return;
	}

	if (result != l2cap_configuration_rsp::RESULT_SUCCESS) {
		// Resend configuration request to try again.
		_SendChannelConfig();
		return;
	}

	// Success response: store RFC parameters if present
	if (rfc != NULL) {
		if (rfc->mode == l2cap_rfc::MODE_ERTM) {
			fErtmMode = l2cap_rfc::MODE_ERTM;
			fTxWindow = rfc->tx_window_size;
			fMaxTransmit = rfc->max_transmit;
			if (rfc->max_pdu_size > 0)
				fMaxPduSize = rfc->max_pdu_size;
			ERROR("l2cap: ERTM confirmed by remote: txwin=%d maxtx=%d maxpdu=%d\n",
				fTxWindow, fMaxTransmit, fMaxPduSize);
		} else {
			fErtmMode = l2cap_rfc::MODE_BASIC;
		}
	}
	if (fcsOption == L2CAP_FCS_CRC16)
		fFcsEnabled = true;
	else if (fcsOption == L2CAP_FCS_NONE)
		fFcsEnabled = false;

	// We now have all options.
	fConfigState.out = ConfigState::DONE;
	ERROR("l2cap: _HandleConfigurationRsp: cid=%d config rsp ok, in=%d mode=%d fcs=%d\n",
		fChannelID, fConfigState.in, fErtmMode, fFcsEnabled);

	if (fConfigState.in == ConfigState::DONE)
		_MarkEstablished();
}


status_t
L2capEndpoint::_MarkEstablished()
{
	CALLED();
	ASSERT_LOCKED_MUTEX(&fLock);
	ERROR("l2cap: _MarkEstablished: cid=%d dcid=%d mode=%d — channel OPEN\n",
		fChannelID, fDestinationChannelID, fErtmMode);

	// Reset ERTM sequence numbers and TX state
	fTxSeq = 0;
	fExpectedTxSeq = 0;
	fReqSeq = 0;
	fExpectedAckSeq = 0;
	fUnackedCount = 0;
	fRemoteBusy = false;
	fWaitF = false;
	for (int i = 0; i < kMaxTxWindow; i++) {
		if (fTxQueue[i].buffer != NULL) {
			gBufferModule->free(fTxQueue[i].buffer);
			fTxQueue[i].buffer = NULL;
		}
	}

	// FCS state was set during config negotiation:
	// - If both sides requested No FCS (FCS=0), fFcsEnabled=false
	// - If neither/one side requested No FCS, fFcsEnabled=true
	// BlueZ: honors remote's FCS=0 by also requesting FCS=0.
	ERROR("l2cap: ERTM: FCS %s\n",
		fFcsEnabled ? "enabled (CRC-16)" : "disabled (No FCS)");

	fState = OPEN;
	fCommandWait.NotifyAll();

	status_t error = B_OK;
	if (gSocketModule->has_parent(socket)) {
		error = gSocketModule->set_connected(socket);
		if (error == B_OK) {
			release_sem(fAcceptSemaphore);
			fAcceptSemaphore = -1;
		} else {
			ERROR("%s: could not set endpoint %p connected: %s\n", __func__, this,
				strerror(error));
		}
	}

	return error;
}


void
L2capEndpoint::_HandleDisconnectionReq(uint8 ident, uint16 scid)
{
	CALLED();
	MutexLocker locker(fLock);
	fCommandWait.NotifyAll();

	if (scid != fDestinationChannelID) {
		ERROR("l2cap: unexpected disconnection req: invalid source channel (cid=%d, scid=%d)\n",
			fChannelID, scid);
		send_l2cap_command_reject(fConnection, ident,
			l2cap_command_reject::REJECTED_INVALID_CID, 0, scid, fChannelID);
		return;
	}

	if (fState != WAIT_FOR_DISCONNECTION_RSP)
		fState = RECEIVED_DISCONNECTION_REQ;

	// The dcid/scid are the same as in the REQ command.
	status_t status = send_l2cap_disconnection_rsp(fConnection, ident, fChannelID, scid);
	if (status != B_OK) {
		// TODO?
		return;
	}

	_MarkClosed();
}


void
L2capEndpoint::_HandleDisconnectionRsp(uint8 ident, uint16 dcid, uint16 scid)
{
	CALLED();
	MutexLocker locker(fLock);
	fCommandWait.NotifyAll();

	if (fState != WAIT_FOR_DISCONNECTION_RSP) {
		ERROR("l2cap: unexpected disconnection rsp (cid=%d, scid=%d)\n",
			fChannelID, scid);
		send_l2cap_command_reject(fConnection, ident,
			l2cap_command_reject::REJECTED_INVALID_CID, 0, scid, fChannelID);
		return;
	}

	if (dcid != fDestinationChannelID && scid != fChannelID) {
		ERROR("l2cap: unexpected disconnection rsp: mismatched CIDs (dcid=%d, scid=%d)\n",
			dcid, scid);
		return;
	}

	_MarkClosed();
}


void
L2capEndpoint::_MarkClosed()
{
	CALLED();
	ASSERT_LOCKED_MUTEX(&fLock);

	// Free ERTM TX queue buffers and stop timers
	for (int i = 0; i < kMaxTxWindow; i++) {
		if (fTxQueue[i].buffer != NULL) {
			gBufferModule->free(fTxQueue[i].buffer);
			fTxQueue[i].buffer = NULL;
		}
	}
	fUnackedCount = 0;
	_StopRetransmitTimer();
	_StopMonitorTimer();

	fState = CLOSED;

	// Wake any thread blocked in ReadData() → fifo_dequeue_buffer() by
	// enqueuing a zero-length EOF marker. The FIFO's notify semaphore is
	// released, so the blocked reader wakes and dequeues the marker. The
	// network stack sees a zero-length buffer and returns 0 (EOF) from
	// recv(), allowing the SDP server (or any L2CAP socket user) to
	// detect the disconnection and accept new connections.
	net_buffer* eofMarker = gBufferModule->create(0);
	if (eofMarker != NULL)
		gStackModule->fifo_enqueue_buffer(&fReceiveQueue, eofMarker);

	gL2capEndpointManager.UnbindFromChannel(this);
}


void
L2capEndpoint::_HandleInfoRsp(uint32 features)
{
	MutexLocker locker(fLock);
	fRemoteExtendedFeatures = features;
	if (fState == WAIT_FOR_INFO_RSP) {
		fState = CLOSED;
		fCommandWait.NotifyAll();
	}
}


// #pragma mark - ERTM retransmission


void
L2capEndpoint::_ProcessAck(uint8 reqSeq)
{
	ASSERT_LOCKED_MUTEX(&fLock);

	bool progress = false;
	while (fExpectedAckSeq != reqSeq && fUnackedCount > 0) {
		uint8 idx = fExpectedAckSeq % kMaxTxWindow;
		if (fTxQueue[idx].buffer != NULL) {
			gBufferModule->free(fTxQueue[idx].buffer);
			fTxQueue[idx].buffer = NULL;
		}
		fExpectedAckSeq = (fExpectedAckSeq + 1) & 0x3F;
		fUnackedCount--;
		progress = true;
	}

	if (fUnackedCount == 0)
		_StopRetransmitTimer();
	else if (progress)
		_StartRetransmitTimer();
	// If no progress and still unacked: don't touch the timer.
	// The caller (S-frame handler) decides whether to retransmit.
}


void
L2capEndpoint::_RetransmitFrom(uint8 fromSeq)
{
	ASSERT_LOCKED_MUTEX(&fLock);

	for (uint8 seq = fromSeq; seq != fTxSeq; seq = (seq + 1) & 0x3F) {
		uint8 idx = seq % kMaxTxWindow;
		if (fTxQueue[idx].buffer == NULL)
			continue;
		if (fTxQueue[idx].retryCount >= fMaxTransmit) {
			ERROR("l2cap: ERTM max_transmit reached seq=%d, disconnecting\n", seq);
			fState = CLOSED;
			fCommandWait.NotifyAll();
			return;
		}
		net_buffer* clone = gBufferModule->clone(fTxQueue[idx].buffer, false);
		if (clone == NULL)
			break;
		fTxQueue[idx].retryCount++;

		ERROR("l2cap: ERTM retransmit seq=%d retry=%d\n",
			seq, fTxQueue[idx].retryCount);

		clone->type = fConnection->handle;
		btDevices->PostACL(fConnection->ndevice->index, clone);
	}
	_StartRetransmitTimer();
}


void
L2capEndpoint::_RetransmitSingle(uint8 seq)
{
	ASSERT_LOCKED_MUTEX(&fLock);

	uint8 idx = seq % kMaxTxWindow;
	if (fTxQueue[idx].buffer == NULL)
		return;
	if (fTxQueue[idx].retryCount >= fMaxTransmit) {
		ERROR("l2cap: ERTM max_transmit reached seq=%d, disconnecting\n", seq);
		fState = CLOSED;
		fCommandWait.NotifyAll();
		return;
	}
	net_buffer* clone = gBufferModule->clone(fTxQueue[idx].buffer, false);
	if (clone == NULL)
		return;
	fTxQueue[idx].retryCount++;

	ERROR("l2cap: ERTM retransmit single seq=%d retry=%d\n",
		seq, fTxQueue[idx].retryCount);

	clone->type = fConnection->handle;
	btDevices->PostACL(fConnection->ndevice->index, clone);
}


// #pragma mark - ERTM timers


/* static */ int32
L2capEndpoint::_RetransmitTimeout(timer* t)
{
	// Runs in interrupt context — cannot acquire mutex or allocate memory.
	// Defer the work to the timer thread via atomic flag + semaphore.
	L2capEndpoint* ep = (L2capEndpoint*)t->user_data;
	atomic_or(&ep->fTimerFlags, kRetransmitPending);
	release_sem_etc(ep->fTimerSem, 1, B_DO_NOT_RESCHEDULE);
	return B_HANDLED_INTERRUPT;
}


/* static */ int32
L2capEndpoint::_MonitorTimeout(timer* t)
{
	// Runs in interrupt context — cannot acquire mutex or allocate memory.
	L2capEndpoint* ep = (L2capEndpoint*)t->user_data;
	atomic_or(&ep->fTimerFlags, kMonitorExpired);
	release_sem_etc(ep->fTimerSem, 1, B_DO_NOT_RESCHEDULE);
	return B_HANDLED_INTERRUPT;
}


/* static */ status_t
L2capEndpoint::_TimerThreadFunc(void* arg)
{
	L2capEndpoint* ep = (L2capEndpoint*)arg;

	while (acquire_sem(ep->fTimerSem) == B_OK) {
		int32 flags = atomic_and(&ep->fTimerFlags, 0);
		if (flags == 0)
			continue;

		MutexLocker locker(ep->fLock);

		if (flags & kRetransmitPending) {
			ep->fRetransmitTimerActive = false;
			if (ep->fState == OPEN) {
				ERROR("l2cap: ERTM retransmit timeout, sending RR+Poll\n");
				ep->_SendErtmSFrame(L2CAP_SFRAME_RR, true, false);
				ep->fWaitF = true;
				ep->_StartMonitorTimer();
			}
		}

		if (flags & kMonitorExpired) {
			ep->fMonitorTimerActive = false;
			if (ep->fState == OPEN) {
				ERROR("l2cap: ERTM monitor timeout — disconnecting\n");
				ep->_MarkClosed();
			}
		}
	}

	return B_OK;
}


void
L2capEndpoint::_StartRetransmitTimer()
{
	if (fRetransmitTimerActive)
		cancel_timer(&fRetransmitTimer);
	fRetransmitTimer.user_data = this;
	add_timer(&fRetransmitTimer, _RetransmitTimeout,
		2000000LL, B_ONE_SHOT_RELATIVE_TIMER);
	fRetransmitTimerActive = true;
}


void
L2capEndpoint::_StopRetransmitTimer()
{
	if (fRetransmitTimerActive) {
		cancel_timer(&fRetransmitTimer);
		fRetransmitTimerActive = false;
	}
}


void
L2capEndpoint::_StartMonitorTimer()
{
	if (fMonitorTimerActive)
		cancel_timer(&fMonitorTimer);
	fMonitorTimer.user_data = this;
	add_timer(&fMonitorTimer, _MonitorTimeout,
		12000000LL, B_ONE_SHOT_RELATIVE_TIMER);
	fMonitorTimerActive = true;
}


void
L2capEndpoint::_StopMonitorTimer()
{
	if (fMonitorTimerActive) {
		cancel_timer(&fMonitorTimer);
		fMonitorTimerActive = false;
	}
}
