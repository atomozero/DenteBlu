/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_spp_test — Interactive SPP (Serial Port Profile) test tool.
 *
 * Client mode:
 *   Connects to a remote Bluetooth device via RFCOMM and provides an
 *   interactive serial console. Type text on Haiku → sent to remote.
 *   Data received from remote → printed to stdout.
 *   Type "quit" to disconnect.
 *   Usage: bt_spp_test <BD_ADDR> [rfcomm_channel]
 *     If rfcomm_channel is omitted, SDP is queried for UUID 0x1101.
 *
 * Server (listen) mode:
 *   Enables Page Scan, listens on L2CAP PSM 3 (RFCOMM), and waits for
 *   a remote device to connect and open an RFCOMM channel (e.g. from
 *   Android Serial Bluetooth Terminal).
 *   Usage: bt_spp_test --listen
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>

#include <Application.h>
#include <OS.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/DeviceClass.h>
#include <bluetooth/LocalDevice.h>
#include <bluetooth/SppClient.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/L2CAP/btL2CAP.h>
#include <l2cap.h>

/* Private header — RfcommSession is inside libbluetooth.so */
#include "RfcommSession.h"


static volatile bool sRunning = true;


/* Background thread that polls for incoming data and prints it */
static status_t
RecvThreadFunc(void* arg)
{
	Bluetooth::SppClient* spp = (Bluetooth::SppClient*)arg;
	uint8 buf[512];

	while (sRunning && spp->IsConnected()) {
		ssize_t received = spp->Receive(buf, sizeof(buf) - 1, 500000LL);
		if (received > 0) {
			buf[received] = '\0';
			printf("%s", (const char*)buf);
			fflush(stdout);
		}
	}

	return B_OK;
}


/* Background thread for server-mode receive */
struct ServerCtx {
	RfcommSession*		session;
	uint8				dlci;
};


static status_t
ServerRecvThreadFunc(void* arg)
{
	ServerCtx* ctx = (ServerCtx*)arg;
	uint8 buf[512];

	while (sRunning && ctx->session->IsConnected()) {
		ssize_t received = ctx->session->Receive(ctx->dlci, buf,
			sizeof(buf) - 1, 500000LL);
		if (received > 0) {
			buf[received] = '\0';
			printf("%s", (const char*)buf);
			fflush(stdout);
		}
	}

	return B_OK;
}


static int
RunListenMode()
{
	/* Enable Page Scan + Inquiry Scan so remote devices can find and
	 * connect to us */
	printf("Enabling Page Scan + Inquiry Scan...\n");
	fflush(stdout);

	LocalDevice* localDev = LocalDevice::GetLocalDevice();
	if (localDev == NULL) {
		fprintf(stderr, "No local Bluetooth device found\n");
		return 1;
	}

	/* Set Class of Device: Computer/Desktop + Networking service */
	DeviceClass devClass(0x01, 0x01, 0x0010);  // major=Computer, minor=Desktop, service=Networking
	localDev->SetDeviceClass(devClass);
	printf("Class of Device set to 0x%06lx\n", (unsigned long)devClass.Record());
	fflush(stdout);

	status_t result = localDev->SetDiscoverable(HCI_SCAN_INQUIRY_PAGE);
	if (result != B_OK) {
		fprintf(stderr, "SetDiscoverable failed: %s\n", strerror(result));
		/* Continue anyway — scan may already be enabled */
	} else {
		printf("Scan enabled. Device is connectable + discoverable.\n");
		fflush(stdout);
	}

	/* Open L2CAP server socket on PSM 3 (RFCOMM) */
	int serverSocket = socket(PF_BLUETOOTH, SOCK_STREAM,
		BLUETOOTH_PROTO_L2CAP);
	if (serverSocket < 0) {
		fprintf(stderr, "socket() failed: %s\n", strerror(errno));
		return 1;
	}

	int reuse = 1;
	setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &reuse,
		sizeof(reuse));

	struct sockaddr_l2cap addr;
	memset(&addr, 0, sizeof(addr));
	addr.l2cap_len = sizeof(addr);
	addr.l2cap_family = AF_BLUETOOTH;
	addr.l2cap_bdaddr = BDADDR_ANY;
	addr.l2cap_psm = B_HOST_TO_LENDIAN_INT16(L2CAP_PSM_RFCOMM);

	if (bind(serverSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "bind(PSM 3) failed: %s\n", strerror(errno));
		close(serverSocket);
		return 1;
	}

	if (listen(serverSocket, 5) < 0) {
		fprintf(stderr, "listen() failed: %s\n", strerror(errno));
		close(serverSocket);
		return 1;
	}

	printf("Listening on L2CAP PSM 3 (RFCOMM)...\n");
	printf("Waiting for remote device to connect...\n");
	fflush(stdout);

	/* Accept one connection */
	struct sockaddr_l2cap remoteAddr;
	uint len = sizeof(remoteAddr);
	int clientSocket = accept(serverSocket, (struct sockaddr*)&remoteAddr,
		&len);
	if (clientSocket < 0) {
		fprintf(stderr, "accept() failed: %s\n", strerror(errno));
		close(serverSocket);
		return 1;
	}

	printf("L2CAP connection accepted from %s\n",
		bdaddrUtils::ToString(remoteAddr.l2cap_bdaddr).String());
	fflush(stdout);

	/* Hand the socket to an RfcommSession in server mode */
	RfcommSession rfcomm;
	result = rfcomm.AcceptFrom(clientSocket);
	if (result != B_OK) {
		fprintf(stderr, "RFCOMM AcceptFrom failed: %s\n", strerror(result));
		close(clientSocket);
		close(serverSocket);
		return 1;
	}

	printf("RFCOMM multiplexer established. Waiting for channel...\n");
	fflush(stdout);

	/* Wait for remote to open a channel (30s) */
	uint8 dlci = rfcomm.WaitForChannel(30000000LL);
	if (dlci == 0) {
		fprintf(stderr, "No RFCOMM channel opened by remote\n");
		rfcomm.Disconnect();
		close(serverSocket);
		return 1;
	}

	printf("RFCOMM channel open on DLCI %u. Interactive mode.\n", dlci);
	printf("Type text to send, 'quit' to exit.\n");
	fflush(stdout);

	/* Start receive thread */
	ServerCtx ctx;
	ctx.session = &rfcomm;
	ctx.dlci = dlci;

	thread_id recvThread = spawn_thread(ServerRecvThreadFunc, "spp_srv_recv",
		B_NORMAL_PRIORITY, &ctx);
	if (recvThread >= 0)
		resume_thread(recvThread);

	/* Interactive send loop */
	char line[1024];
	while (sRunning && rfcomm.IsConnected()) {
		if (fgets(line, sizeof(line), stdin) == NULL)
			break;

		size_t lineLen = strlen(line);
		if (lineLen >= 4 && strncmp(line, "quit", 4) == 0)
			break;

		ssize_t sent = rfcomm.Send(dlci, line, lineLen);
		if (sent < 0) {
			fprintf(stderr, "Send failed: %s\n", strerror(sent));
			break;
		}
	}

	printf("\nDisconnecting...\n");
	sRunning = false;
	rfcomm.Disconnect();
	close(serverSocket);

	if (recvThread >= 0) {
		status_t dummy;
		wait_for_thread(recvThread, &dummy);
	}

	printf("Done.\n");
	return 0;
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr,
			"Usage: %s <BD_ADDR> [rfcomm_channel]\n"
			"       %s --listen\n"
			"  e.g. %s 0C:7D:B0:B2:81:6A\n"
			"       %s 0C:7D:B0:B2:81:6A 1\n"
			"       %s --listen  (wait for incoming connection)\n",
			argv[0], argv[0], argv[0], argv[0], argv[0]);
		return 1;
	}

	/* BApplication needed for BMessenger IPC */
	BApplication app("application/x-vnd.Haiku-bt_spp_test");

	/* Server (listen) mode */
	if (strcmp(argv[1], "--listen") == 0)
		return RunListenMode();

	/* Client mode */
	bdaddr_t remote = bdaddrUtils::FromString(argv[1]);
	if (bdaddrUtils::Compare(remote, bdaddrUtils::NullAddress())) {
		fprintf(stderr, "Invalid BD_ADDR: %s\n", argv[1]);
		return 1;
	}

	uint8 channel = 0;
	if (argc >= 3) {
		channel = (uint8)atoi(argv[2]);
		if (channel < 1 || channel > 30) {
			fprintf(stderr, "Invalid RFCOMM channel: %s (must be 1-30)\n",
				argv[2]);
			return 1;
		}
	}

	printf("SPP Test — connecting to %s", argv[1]);
	if (channel > 0)
		printf(" channel %u", channel);
	else
		printf(" (SDP auto-detect)");
	printf("...\n");
	fflush(stdout);

	Bluetooth::SppClient spp;
	status_t result = spp.Connect(remote, channel);
	if (result != B_OK) {
		fprintf(stderr, "Connect failed: %s\n", strerror(result));
		return 1;
	}

	printf("Connected! MTU=%u. Type text to send, 'quit' to exit.\n",
		spp.Mtu());
	fflush(stdout);

	/* Start receive thread */
	thread_id recvThread = spawn_thread(RecvThreadFunc, "spp_recv",
		B_NORMAL_PRIORITY, &spp);
	if (recvThread >= 0)
		resume_thread(recvThread);

	/* Interactive send loop: read lines from stdin */
	char line[1024];
	while (sRunning && spp.IsConnected()) {
		if (fgets(line, sizeof(line), stdin) == NULL)
			break;

		/* Check for quit command */
		size_t len = strlen(line);
		if (len >= 4 && strncmp(line, "quit", 4) == 0)
			break;

		/* Send the line (including newline) */
		ssize_t sent = spp.Send(line, len);
		if (sent < 0) {
			fprintf(stderr, "Send failed: %s\n", strerror(sent));
			break;
		}
	}

	printf("\nDisconnecting...\n");
	sRunning = false;
	spp.Disconnect();

	if (recvThread >= 0) {
		status_t dummy;
		wait_for_thread(recvThread, &dummy);
	}

	printf("Done.\n");
	return 0;
}
