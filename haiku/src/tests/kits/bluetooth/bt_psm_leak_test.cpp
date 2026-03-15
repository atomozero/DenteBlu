/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_psm_leak_test — Tests that L2CAP PSM bindings are released when
 * a process is killed with SIGKILL.
 *
 * 1. Forks a child process
 * 2. Child binds an L2CAP socket to a dynamic PSM
 * 3. Parent sends SIGKILL to the child
 * 4. Parent waits for child to die
 * 5. Parent tries to bind to the same PSM
 * 6. If bind succeeds → PASS (PSM was released)
 *    If EADDRINUSE → FAIL (PSM leaked)
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/L2CAP/btL2CAP.h>
#include <l2cap.h>


/* Dynamic PSM for testing (must be odd, >= 0x1001) */
#define TEST_PSM	0x1001


static int
BindL2capSocket(uint16 psm)
{
	int sock = socket(PF_BLUETOOTH, SOCK_STREAM, BLUETOOTH_PROTO_L2CAP);
	if (sock < 0) {
		fprintf(stderr, "  socket() failed: %s\n", strerror(errno));
		return -1;
	}

	struct sockaddr_l2cap addr;
	memset(&addr, 0, sizeof(addr));
	addr.l2cap_len = sizeof(addr);
	addr.l2cap_family = PF_BLUETOOTH;
	addr.l2cap_psm = psm;
	/* Bind to BDADDR_ANY */

	if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		int err = errno;
		fprintf(stderr, "  bind(PSM 0x%04X) failed: %s\n", psm,
			strerror(err));
		close(sock);
		errno = err;
		return -1;
	}

	return sock;
}


int
main()
{
	printf("=== L2CAP PSM Leak Test ===\n\n");

	/* Step 1: Fork child process */
	printf("Step 1: Forking child process...\n");

	/* Use a pipe to synchronize: child writes 'R' when ready */
	int pipeFd[2];
	if (pipe(pipeFd) < 0) {
		fprintf(stderr, "pipe() failed: %s\n", strerror(errno));
		return 1;
	}

	pid_t child = fork();
	if (child < 0) {
		fprintf(stderr, "fork() failed: %s\n", strerror(errno));
		return 1;
	}

	if (child == 0) {
		/* Child process */
		close(pipeFd[0]);  /* close read end */

		int sock = BindL2capSocket(TEST_PSM);
		if (sock < 0) {
			write(pipeFd[1], "E", 1);
			close(pipeFd[1]);
			_exit(1);
		}

		printf("  Child (pid %d): bound PSM 0x%04X, waiting for SIGKILL...\n",
			getpid(), TEST_PSM);
		fflush(stdout);

		/* Signal parent that we're ready */
		write(pipeFd[1], "R", 1);
		close(pipeFd[1]);

		/* Sleep forever — parent will SIGKILL us */
		while (1)
			sleep(60);

		/* Never reached */
		close(sock);
		_exit(0);
	}

	/* Parent process */
	close(pipeFd[1]);  /* close write end */

	/* Wait for child to be ready */
	char readyBuf;
	ssize_t n = read(pipeFd[0], &readyBuf, 1);
	close(pipeFd[0]);

	if (n != 1 || readyBuf != 'R') {
		fprintf(stderr, "Child failed to bind PSM\n");
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 1;
	}

	printf("Step 2: Child bound PSM 0x%04X successfully.\n", TEST_PSM);

	/* Step 3: Kill the child */
	printf("Step 3: Sending SIGKILL to child (pid %d)...\n", child);
	kill(child, SIGKILL);

	/* Step 4: Wait for child to die */
	int status;
	waitpid(child, &status, 0);
	printf("Step 4: Child terminated (status=%d, signal=%d)\n",
		WEXITSTATUS(status), WIFSIGNALED(status) ? WTERMSIG(status) : 0);

	/* Give the kernel a moment to clean up socket references */
	usleep(500000);  /* 500ms */

	/* Step 5: Try to bind the same PSM */
	printf("Step 5: Parent trying to bind PSM 0x%04X...\n", TEST_PSM);

	int sock = BindL2capSocket(TEST_PSM);
	if (sock >= 0) {
		printf("\n=== PASS: PSM 0x%04X was released after SIGKILL ===\n",
			TEST_PSM);
		close(sock);
		return 0;
	} else if (errno == EADDRINUSE) {
		printf("\n=== FAIL: PSM 0x%04X still bound (leaked!) ===\n",
			TEST_PSM);
		return 1;
	} else {
		printf("\n=== ERROR: Unexpected bind error: %s ===\n",
			strerror(errno));
		return 1;
	}
}
