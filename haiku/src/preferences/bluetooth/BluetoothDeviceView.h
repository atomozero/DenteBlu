/*
 * Copyright 2008-09, Oliver Ruiz Dorantes, <oliver.ruiz.dorantes_at_gmail.com>
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef BLUETOOTHDEVICEVIEW_H_
#define BLUETOOTHDEVICEVIEW_H_

#include <Box.h>
#include <Bitmap.h>
#include <Invoker.h>
#include <Message.h>
#include <View.h>

#include <bluetooth/BluetoothDevice.h>


class BStringView;
class BitmapView;


class DeviceIconView : public BView {
public:
	DeviceIconView()
		:
		BView("Icon", B_WILL_DRAW),
		fClass()
	{
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		float size = DeviceClass::PixelsForIcon + 2 * DeviceClass::IconInsets;
		SetExplicitSize(BSize(size, size));
		SetExplicitAlignment(BAlignment(B_ALIGN_LEFT, B_ALIGN_TOP));
	}

	void SetDeviceClass(const DeviceClass& deviceClass)
	{
		fClass = deviceClass;
		Invalidate();
	}

	void Draw(BRect updateRect)
	{
		fClass.Draw(this, BPoint(0, 0));
	}

private:
	DeviceClass	fClass;
};


class BluetoothDeviceView : public BView
{
public:
	BluetoothDeviceView(BluetoothDevice* bDevice,
		uint32 flags = B_WILL_DRAW);
	~BluetoothDeviceView();

			void SetBluetoothDevice(BluetoothDevice* bDevice);
			void SetDeviceInfo(const char* name, const char* address,
					const char* description);

	virtual void MessageReceived(BMessage* message);
	virtual void SetTarget(BHandler* target);
	virtual void SetEnabled(bool value);

protected:
	BluetoothDevice*	fDevice;

	BStringView*		fName;
	BStringView*		fBdaddr;
	BStringView*		fClassService;
	BStringView*		fClass;

	BStringView*		fHCIVersionProperties;
	BStringView*		fLMPVersionProperties;
	BStringView*		fManufacturerProperties;

	BStringView*		fACLBuffersProperties;
	BStringView*		fSCOBuffersProperties;

	DeviceIconView*		fIcon;
};


#endif
