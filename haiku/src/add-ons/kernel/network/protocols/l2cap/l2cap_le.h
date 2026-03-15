/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef L2CAP_LE_H
#define L2CAP_LE_H

#include <l2cap.h>

struct HciConnection;
struct net_buffer;


status_t att_receive_data(struct HciConnection* conn, net_buffer* buffer);
status_t smp_receive_data(struct HciConnection* conn, net_buffer* buffer);
status_t l2cap_handle_le_signaling_command(struct HciConnection* conn,
	net_buffer* buffer);

typedef status_t (*att_receive_callback)(void* channel, net_buffer* buffer);
typedef status_t (*smp_receive_callback)(void* manager, net_buffer* buffer);

void l2cap_le_set_att_callback(att_receive_callback callback);
void l2cap_le_set_smp_callback(smp_receive_callback callback);


#endif /* L2CAP_LE_H */
