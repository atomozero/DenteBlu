/*
 * Copyright 2008-09, Oliver Ruiz Dorantes, <oliver.ruiz.dorantes_at_gmail.com>
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#include "BluetoothDeviceView.h"
#include <bluetooth/bdaddrUtils.h>

#include <bluetooth/LocalDevice.h>
#include <bluetooth/HCI/btHCI_command.h>


#include <Bitmap.h>
#include <Catalog.h>
#include <LayoutBuilder.h>
#include <Locale.h>
#include <SpaceLayoutItem.h>
#include <StringView.h>
#include <TextView.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Device View"

BluetoothDeviceView::BluetoothDeviceView(BluetoothDevice* bDevice, uint32 flags)
	:
	BView("BluetoothDeviceView", flags | B_WILL_DRAW),
	fDevice(bDevice)
{
	fName = new BStringView("name", "");
	fName->SetFont(be_bold_font);
	fName->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT, B_ALIGN_MIDDLE));

	fBdaddr = new BStringView("bdaddr",
		bdaddrUtils::ToString(bdaddrUtils::NullAddress()));
	fBdaddr->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT, B_ALIGN_MIDDLE));

	fClassService = new BStringView("ServiceClass",
		B_TRANSLATE("Service classes: "));
	fClassService->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT,
		B_ALIGN_MIDDLE));

	fClass = new BStringView("class", "- / -");
	fClass->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT, B_ALIGN_MIDDLE));

	fHCIVersionProperties = new BStringView("hci", "");
	fHCIVersionProperties->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT,
		B_ALIGN_MIDDLE));
	fLMPVersionProperties = new BStringView("lmp", "");
	fLMPVersionProperties->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT,
		B_ALIGN_MIDDLE));
	fManufacturerProperties = new BStringView("manufacturer", "");
	fManufacturerProperties->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT,
		B_ALIGN_MIDDLE));
	fACLBuffersProperties = new BStringView("buffers acl", "");
	fACLBuffersProperties->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT,
		B_ALIGN_MIDDLE));
	fSCOBuffersProperties = new BStringView("buffers sco", "");
	fSCOBuffersProperties->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT,
		B_ALIGN_MIDDLE));

	fIcon = new DeviceIconView();

	SetBluetoothDevice(bDevice);

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_HALF_ITEM_SPACING)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
			.Add(fIcon)
			.AddGroup(B_VERTICAL, 0)
				.Add(fName)
				.Add(fBdaddr)
			.End()
			.AddGlue()
		.End()
		.AddGrid(B_USE_DEFAULT_SPACING, 0)
			.Add(fClass, 0, 0)
			.Add(fClassService, 1, 0)
			.Add(fHCIVersionProperties, 0, 1)
			.Add(fLMPVersionProperties, 1, 1)
			.Add(fManufacturerProperties, 0, 2, 2)
			.Add(fACLBuffersProperties, 0, 3)
			.Add(fSCOBuffersProperties, 1, 3)
		.End()
	.End();

	SetExplicitAlignment(BAlignment(B_ALIGN_USE_FULL_WIDTH,
		B_ALIGN_VERTICAL_UNSET));
}


BluetoothDeviceView::~BluetoothDeviceView()
{
}


static void
_ShowStringView(BStringView* view, const char* text)
{
	view->SetText(text);
	if (text != NULL && text[0] != '\0') {
		if (view->IsHidden())
			view->Show();
	} else {
		if (!view->IsHidden())
			view->Hide();
	}
}


void
BluetoothDeviceView::SetBluetoothDevice(BluetoothDevice* bDevice)
{
	if (bDevice != NULL) {
		SetName(bDevice->GetFriendlyName().String());

		_ShowStringView(fName, bDevice->GetFriendlyName().String());
		_ShowStringView(fBdaddr,
			bdaddrUtils::ToString(bDevice->GetBluetoothAddress()));

		BString string(B_TRANSLATE("Service classes: "));
		bDevice->GetDeviceClass().GetServiceClass(string);
		_ShowStringView(fClassService, string.String());

		string = "";
		bDevice->GetDeviceClass().GetMajorDeviceClass(string);
		string << " / ";
		bDevice->GetDeviceClass().GetMinorDeviceClass(string);
		_ShowStringView(fClass, string.String());

		fIcon->SetDeviceClass(bDevice->GetDeviceClass());

		uint32 value;

		string = "";
		if (bDevice->GetProperty("hci_version", &value) == B_OK)
			string << "HCI ver: " << BluetoothHciVersion(value);
		if (bDevice->GetProperty("hci_revision", &value) == B_OK)
			string << " HCI rev: " << value ;
		_ShowStringView(fHCIVersionProperties, string.String());

		string = "";
		if (bDevice->GetProperty("lmp_version", &value) == B_OK)
			string << "LMP ver: " << BluetoothLmpVersion(value);
		if (bDevice->GetProperty("lmp_subversion", &value) == B_OK)
			string << " LMP subver: " << value;
		_ShowStringView(fLMPVersionProperties, string.String());

		string = "";
		if (bDevice->GetProperty("manufacturer", &value) == B_OK)
			string << B_TRANSLATE("Manufacturer: ")
			   	<< BluetoothManufacturer(value);
		_ShowStringView(fManufacturerProperties, string.String());

		string = "";
		if (bDevice->GetProperty("acl_mtu", &value) == B_OK)
			string << "ACL mtu: " << value;
		if (bDevice->GetProperty("acl_max_pkt", &value) == B_OK)
			string << B_TRANSLATE(" packets: ") << value;
		_ShowStringView(fACLBuffersProperties, string.String());

		string = "";
		if (bDevice->GetProperty("sco_mtu", &value) == B_OK)
			string << "SCO mtu: " << value;
		if (bDevice->GetProperty("sco_max_pkt", &value) == B_OK)
			string << B_TRANSLATE(" packets: ") << value;
		_ShowStringView(fSCOBuffersProperties, string.String());
	}
}


void
BluetoothDeviceView::SetDeviceInfo(const char* name, const char* address,
	const char* description)
{
	_ShowStringView(fName, name);
	_ShowStringView(fBdaddr, address);
	_ShowStringView(fClassService, description);
	_ShowStringView(fClass, NULL);
	_ShowStringView(fHCIVersionProperties, NULL);
	_ShowStringView(fLMPVersionProperties, NULL);
	_ShowStringView(fManufacturerProperties, NULL);
	_ShowStringView(fACLBuffersProperties, NULL);
	_ShowStringView(fSCOBuffersProperties, NULL);
}


void
BluetoothDeviceView::SetTarget(BHandler* target)
{
}


void
BluetoothDeviceView::MessageReceived(BMessage* message)
{
	// If we received a dropped message, try to see if it has color data
	// in it
	if (message->WasDropped()) {

	}

	// The default
	BView::MessageReceived(message);
}


void
BluetoothDeviceView::SetEnabled(bool value)
{
}
