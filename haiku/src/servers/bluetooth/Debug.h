/*
 * Copyright 2016, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Augustin Cavalier, <waddlesplash>
 */
#ifndef _BLUETOOTH_SERVER_DEBUG_H
#define _BLUETOOTH_SERVER_DEBUG_H

//#ifdef TRACE_BLUETOOTH_SERVER
#if 1
#	define TRACE_BT(x...) do { \
		FILE* _f = fopen("/tmp/bt_server.log", "a"); \
		if (_f) { fprintf(_f, x); fclose(_f); } \
	} while (0)
#else
#	define TRACE_BT(x)
#endif

#endif
