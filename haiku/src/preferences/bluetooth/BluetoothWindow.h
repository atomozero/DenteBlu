/*
 * Copyright 2008-09, Oliver Ruiz Dorantes, <oliver.ruiz.dorantes_at_gmail.com>
 * Copyright 2025, Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BLUETOOTH_WINDOW_H
#define BLUETOOTH_WINDOW_H

#include <Application.h>
#include <Button.h>
#include <ListItem.h>
#include <OutlineListView.h>
#include <ScrollView.h>
#include <StatusBar.h>
#include <Window.h>
#include <Message.h>

#include <bluetooth/DeviceClass.h>
#include <bluetooth/LocalDevice.h>

class BluetoothSettingsView;
class BluetoothDeviceView;
class ExtendedLocalDeviceView;
class PairedDeviceItem;
namespace Bluetooth {
class DeviceListItem;
class DiscoveryAgent;
class DiscoveryListener;
}

class BluetoothWindow : public BWindow {
public:
					BluetoothWindow(BRect frame);
					~BluetoothWindow();
	bool			QuitRequested();
	void			MessageReceived(BMessage* message);

private:
	void			_BuildSidebar();
	void			_SelectItem(BListItem* item);
	void			_ShowAdapterDetail(LocalDevice* device);
	void			_ShowDeviceDetail(Bluetooth::DeviceListItem* item);
	void			_ShowPairedDeviceDetail(PairedDeviceItem* item);
	void			_ClearDetail();
	void			_ClearDevices();
	void			_UpdateButtonsForClass(DeviceClass devClass);

	// Sidebar
	BOutlineListView*		fListView;
	BScrollView*			fScrollView;
	BStringItem*			fAdaptersTitle;
	BStringItem*			fDevicesTitle;

	// Scan progress
	BStatusBar*				fScanProgress;
	rgb_color				fScanActiveColor;

	// Discovery
	Bluetooth::DiscoveryAgent*		fDiscoveryAgent;
	Bluetooth::DiscoveryListener*	fDiscoveryListener;
	bool					fScanning;
	bool					fRetrieving;
	int32					fRetrievalIndex;
	bool					fRetrievalLabelPlaced;
	float					fScanTimer;
	float					fScanElapsed;
	BMessage*				fSecondsMessage;
	BMessage*				fRetrieveMessage;

	// Detail container
	BView*					fDetailView;

	// Adapter detail (reuses BluetoothSettingsView)
	BluetoothSettingsView*	fSettingsView;

	// Device detail (built once, switched in/out)
	BView*					fDeviceDetailView;
	BluetoothDeviceView*	fDeviceInfoView;
	BButton*				fPairButton;
	BButton*				fDisconnectButton;
	BButton*				fServicesButton;
	BButton*				fTerminalButton;
	BButton*				fFileTransferButton;
	BButton*				fPbapButton;
	BButton*				fCallButton;
	BButton*				fA2dpButton;

	// Bottom bar
	BButton*				fDefaultsButton;
	BButton*				fRevertButton;
	BMenuBar*				fMenubar;
};

#endif
