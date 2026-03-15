/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * RfcommSession — RFCOMM multiplexer session over L2CAP.
 * Private to libbluetooth.so; not installed as a public header.
 */
#ifndef _RFCOMM_SESSION_H_
#define _RFCOMM_SESSION_H_

#include <OS.h>
#include <SupportDefs.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>


/* Maximum number of DLCIs (0-61, index 0 is mux control) */
#define RFCOMM_MAX_DLCIS		62

/* Per-DLCI receive buffer size (must be >= RFCOMM_DEFAULT_MTU) */
#define RFCOMM_RECV_BUF_SIZE	8192


/* DLCI states */
enum rfcomm_dlci_state {
	RFCOMM_DLCI_CLOSED = 0,
	RFCOMM_DLCI_OPENING,
	RFCOMM_DLCI_OPEN,
	RFCOMM_DLCI_CLOSING
};


/* Per-DLCI state */
struct RfcommDlciInfo {
	rfcomm_dlci_state	state;
	sem_id				waitSem;		/* signaled on state change or data */
	uint8				recvBuf[RFCOMM_RECV_BUF_SIZE];
	uint32				recvHead;		/* write position (producer) */
	uint32				recvTail;		/* read position (consumer) */
	uint16				mtu;			/* negotiated MTU for this DLC */
	bool				cbfc;			/* credit-based flow control active */
	int32				localCredits;	/* credits remote gave us (we can send) */
	int32				remoteCredits;	/* credits we gave remote (it can send) */
};


class RfcommSession {
public:
								RfcommSession();
								~RfcommSession();

	/* Connection lifecycle (client — initiator) */
	status_t					Connect(const bdaddr_t& remote);
	void						Disconnect();
	bool						IsConnected() const { return fMuxUp; }

	/* Server mode — responder (takes an already-accepted L2CAP socket) */
	status_t					AcceptFrom(int connectedSocket);
	uint8						WaitForChannel(bigtime_t timeout);

	/* Channel management */
	status_t					OpenChannel(uint8 serverChannel);
	void						CloseChannel(uint8 dlci);

	/* Data I/O */
	ssize_t						Send(uint8 dlci, const void* data,
									size_t length);
	ssize_t						Receive(uint8 dlci, void* buffer,
									size_t maxLength, bigtime_t timeout);

	/* Accessors */
	uint16						Mtu() const { return fMtu; }

private:
	/* Frame sending */
	status_t					_SendFrame(uint8 dlci, uint8 control,
									const uint8* data, uint16 dataLen);
	status_t					_SendSABM(uint8 dlci);
	status_t					_SendUA(uint8 dlci);
	status_t					_SendDISC(uint8 dlci);
	status_t					_SendUIH(uint8 dlci, const uint8* data,
									uint16 dataLen);
	status_t					_SendUIHCredits(uint8 dlci,
									uint8 credits, const uint8* data,
									uint16 dataLen);

	/* Multiplexer commands (UIH on DLCI 0) */
	status_t					_SendPN(uint8 dlci, uint16 mtu);
	status_t					_SendMSC(uint8 dlci, uint8 signals);
	void						_GrantCredits(uint8 dlci, uint8 credits);

	/* FCS computation */
	static uint8				_ComputeFCS(const uint8* data, uint8 len);
	static bool					_CheckFCS(const uint8* data, uint8 len,
									uint8 fcs);

	/* Receive thread */
	static status_t				_RecvLoopEntry(void* arg);
	void						_RecvLoop();
	ssize_t						_ParseFrame(const uint8* buf, ssize_t len);
	void						_HandleMuxCommand(const uint8* data,
									uint16 len);

	/* Buffer helpers */
	void						_EnqueueData(uint8 dlci, const uint8* data,
									uint16 len);

	/* State */
	int							fSocket;
	bool						fMuxUp;
	bool						fRunning;
	bool						fIsInitiator;
	uint16						fMtu;
	sem_id						fMuxOpenSem;
	sem_id						fChannelOpenSem;
	uint8						fChannelOpenDlci;
	thread_id					fRecvThread;
	RfcommDlciInfo				fDlcis[RFCOMM_MAX_DLCIS];

	/* FCS lookup table */
	static const uint8			sFcsTable[256];
};


#endif /* _RFCOMM_SESSION_H_ */
