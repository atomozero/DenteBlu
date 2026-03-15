/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * AvrcpTarget — AVRCP Target role: handles headphone button events
 * (play, pause, stop, forward, backward) and absolute volume.
 */
#ifndef _AVRCP_TARGET_H
#define _AVRCP_TARGET_H

#include <OS.h>
#include <SupportDefs.h>
#include <bluetooth/bluetooth.h>


namespace Bluetooth {


/* AVRCP pass-through operation IDs */
enum avrcp_op_id {
	AVRCP_OP_PLAY		= 0x44,
	AVRCP_OP_STOP		= 0x45,
	AVRCP_OP_PAUSE		= 0x46,
	AVRCP_OP_FORWARD	= 0x4B,
	AVRCP_OP_BACKWARD	= 0x4C,
	AVRCP_OP_VOLUME_UP	= 0x41,
	AVRCP_OP_VOLUME_DOWN = 0x42,
};


typedef void (*avrcp_button_callback)(avrcp_op_id op, bool pressed,
	void* cookie);
typedef void (*avrcp_volume_callback)(uint8 volume, void* cookie);


class AvrcpTarget {
public:
							AvrcpTarget();
	virtual					~AvrcpTarget();

	status_t				Connect(const bdaddr_t& address);
	void					Disconnect();
	bool					IsConnected() const;

	void					SetButtonCallback(
								avrcp_button_callback callback,
								void* cookie);
	void					SetVolumeCallback(
								avrcp_volume_callback callback,
								void* cookie);

	status_t				NotifyVolumeChange(uint8 volume);

private:
	static bool				_EnsureAclConnection(
								const bdaddr_t& remote);
	static status_t			_RecvThreadEntry(void* arg);
	void					_RecvLoop();
	void					_HandlePassThrough(uint8 txLabel,
								const uint8* operand, size_t len);
	void					_HandleVendorDependent(uint8 txLabel,
								uint8 ctype, const uint8* operand,
								size_t len);
	ssize_t					_SendResponse(uint8 txLabel,
								uint8 responseCode, uint8 subunit,
								uint8 opcode, const uint8* operand,
								size_t operandLen);

	int						fSocket;
	bdaddr_t				fRemoteAddr;
	bool					fConnected;
	thread_id				fRecvThread;

	avrcp_button_callback	fButtonCallback;
	void*					fButtonCookie;
	avrcp_volume_callback	fVolumeCallback;
	void*					fVolumeCookie;
};


} /* namespace Bluetooth */


#endif /* _AVRCP_TARGET_H */
