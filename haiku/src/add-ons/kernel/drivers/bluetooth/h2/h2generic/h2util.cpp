/*
 * Copyright 2007 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * Copyright 2008 Mika Lindqvist, monni1995_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include "h2util.h"

#include <malloc.h>

#include <bluetooth/HCI/btHCI_acl.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/HCI/btHCI_sco.h>

#include "h2debug.h"
#include "h2upper.h"
#include "h2transactions.h"


void*
nb_get_whole_buffer(net_buffer* nbuf)
{
	void* conPointer;
	status_t err;

	err = nb->direct_access(nbuf, 0, nbuf->size, &conPointer);
	if (err == B_OK)
		return conPointer;

	// Buffer is non-contiguous (e.g. after NetBufferPrepend added a new
	// chunk).  Allocate a flat copy suitable for USB DMA.
	conPointer = malloc(nbuf->size);
	if (conPointer == NULL)
		return NULL;

	err = nb->read(nbuf, 0, conPointer, nbuf->size);
	if (err != B_OK) {
		free(conPointer);
		return NULL;
	}

	return conPointer;
}


void
nb_destroy(net_buffer* nbuf)
{
	if (nbuf == NULL)
		return;

	// Detect double-free: check for freed-memory poison pattern.
	// Haiku fills freed memory with 0xdeadbeef. If we see this pattern
	// in the net_buffer fields, it was already freed.
	if ((uintptr_t)nbuf->link.next == 0xdeadbeefdeadbeefULL
		|| (uintptr_t)nbuf->link.prev == 0xdeadbeefdeadbeefULL) {
		panic("bt nb_destroy: double-free! nbuf=%p link.next=%p "
			"link.prev=%p size=0x%" B_PRIx32 " type=0x%" B_PRIx32
			" protocol=%d",
			nbuf, nbuf->link.next, nbuf->link.prev,
			nbuf->size, (uint32)nbuf->type, nbuf->protocol);
		return;
	}

	if (nb != NULL)
		nb->free(nbuf);
}


// Extract the expected size of the packet
// TODO: This might be inefficient as at the moment of the creation
// of the net_buffer this information is known and it could be stored 
#if 0
ssize_t
get_expected_size(net_buffer* nbuf)
{

	if (nbuf == NULL)
		panic("Analizing NULL packet");

	switch (nbuf->protocol) {

		case BT_COMMAND: {
			struct hci_command_header* header = nb_get_whole_buffer(nbuf);
			return header->clen + sizeof(struct hci_command_header);
		}

		case BT_EVENT: {
			struct hci_event_header* header = nb_get_whole_buffer(nbuf);
			return header->elen + sizeof(struct hci_event_header);
		}

		case BT_ACL: {
			struct hci_acl_header* header = nb_get_whole_buffer(nbuf);
			return header->alen + sizeof(struct hci_acl_header);
		}

		case BT_SCO: {
			struct hci_sco_header* header = nb_get_whole_buffer(nbuf);
			return header->slen + sizeof(struct hci_sco_header);
		}

		default:
			panic(BLUETOOTH_DEVICE_DEVFS_NAME ":no protocol specified for ");
		break;
	}

	return -1;
}
#endif


#if 0
#pragma mark - room util -
#endif


void
init_room(struct list* l)
{
	list_init(l);
}


void*
alloc_room(struct list* l, size_t size)
{
	void* item = list_remove_head_item(l);

	if (item == NULL)
		item = (void*) malloc(size);

	return item;
}


void
reuse_room(struct list* l, void* room)
{
	list_add_item(l, room);
}


void
purge_room(struct list* l)
{
	void* item;

	while ((item = list_remove_head_item(l)) != NULL) {
		free(item);
	}
}
