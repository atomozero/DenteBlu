/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "BluetoothAudioAddOn.h"

#include "BluetoothAudioNode.h"

#include <MediaDefs.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


BluetoothAudioAddOn::BluetoothAudioAddOn(image_id image)
	:
	BMediaAddOn(image)
{
	fNodeCreated = false;
	FILE* f = fopen("/tmp/bt_audio.log", "a");
	if (f) { fprintf(f, "BluetoothAudioAddOn: loaded\n"); fclose(f); }
	memset(&fInputFormat, 0, sizeof(fInputFormat));
	fInputFormat.type = B_MEDIA_RAW_AUDIO;
	fInputFormat.u.raw_audio = media_raw_audio_format::wildcard;

	fFlavorInfo.name = "Bluetooth Audio Output";
	fFlavorInfo.info = "Sends audio to Bluetooth A2DP headphones";
	fFlavorInfo.kinds = B_BUFFER_CONSUMER | B_CONTROLLABLE
		| B_PHYSICAL_OUTPUT;
	fFlavorInfo.flavor_flags = B_FLAVOR_IS_GLOBAL;
	fFlavorInfo.internal_id = 0;
	fFlavorInfo.possible_count = 1;
	fFlavorInfo.in_format_count = 1;
	fFlavorInfo.in_format_flags = 0;
	fFlavorInfo.in_formats = &fInputFormat;
	fFlavorInfo.out_format_count = 0;
	fFlavorInfo.out_format_flags = 0;
	fFlavorInfo.out_formats = NULL;
}


BluetoothAudioAddOn::~BluetoothAudioAddOn()
{
	if (fNodeCreated)
		unlink("/tmp/.bt_audio_node_lock");
}


status_t
BluetoothAudioAddOn::InitCheck(const char** _failureText)
{
	return B_OK;
}


int32
BluetoothAudioAddOn::CountFlavors()
{
	return 1;
}


status_t
BluetoothAudioAddOn::GetFlavorAt(int32 index, const flavor_info** _info)
{
	if (index != 0)
		return B_BAD_INDEX;

	*_info = &fFlavorInfo;
	return B_OK;
}


BMediaNode*
BluetoothAudioAddOn::InstantiateNodeFor(const flavor_info* info,
	BMessage* config, status_t* _error)
{
	FILE* f = fopen("/tmp/bt_audio.log", "a");
	if (f) { fprintf(f, "BluetoothAudioAddOn: InstantiateNodeFor (created=%d)\n",
		fNodeCreated); fclose(f); }

	/* Only allow one global instance — use a file lock to prevent
	 * multiple add-on images from creating duplicate nodes */
	int lockFd = open("/tmp/.bt_audio_node_lock", O_CREAT | O_EXCL | O_WRONLY, 0644);
	if (lockFd < 0 && errno == EEXIST) {
		if (f) { f = fopen("/tmp/bt_audio.log", "a");
			if (f) { fprintf(f, "BluetoothAudioAddOn: SKIP — already instantiated\n"); fclose(f); } }
		*_error = B_NOT_ALLOWED;
		return NULL;
	}
	if (lockFd >= 0)
		close(lockFd);

	BluetoothAudioNode* node = new(std::nothrow) BluetoothAudioNode(this);
	if (node == NULL) {
		unlink("/tmp/.bt_audio_node_lock");
		*_error = B_NO_MEMORY;
		return NULL;
	}

	fNodeCreated = true;
	*_error = B_OK;
	return node;
}


BMediaAddOn*
make_media_addon(image_id image)
{
	return new BluetoothAudioAddOn(image);
}
