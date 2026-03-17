/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * BluetoothAudioAddOn — BMediaAddOn che registra il nodo
 * "Bluetooth Audio Output" nel Media Kit di Haiku.
 */
#ifndef _BLUETOOTH_AUDIO_ADDON_H_
#define _BLUETOOTH_AUDIO_ADDON_H_


#include <MediaAddOn.h>


class BluetoothAudioAddOn : public BMediaAddOn {
public:
								BluetoothAudioAddOn(image_id image);
	virtual						~BluetoothAudioAddOn();

	virtual	status_t			InitCheck(const char** _failureText);
	virtual	int32				CountFlavors();
	virtual	status_t			GetFlavorAt(int32 index,
									const flavor_info** _info);
	virtual	BMediaNode*			InstantiateNodeFor(const flavor_info* info,
									BMessage* config, status_t* _error);

private:
			flavor_info			fFlavorInfo;
			media_format		fInputFormat;
};


#endif /* _BLUETOOTH_AUDIO_ADDON_H_ */
