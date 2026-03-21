/*
 * Copyright 2009, Oliver Ruiz Dorantes, <oliver.ruiz.dorantes_at_gmail.com>
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <Bitmap.h>
#include <ControlLook.h>
#include <View.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/BluetoothDevice.h>

#include "DeviceListItem.h"

#define TEXT_ROWS  2

namespace Bluetooth {

DeviceListItem::DeviceListItem(BluetoothDevice* bDevice)
	:
	BListItem(),
	fDevice(bDevice),
	fName("unknown")
{
	fAddress = bDevice->GetBluetoothAddress();
	fClass = bDevice->GetDeviceClass();
}


void
DeviceListItem::SetDevice(BluetoothDevice* bDevice)
{
	fAddress = bDevice->GetBluetoothAddress();
	fClass = bDevice->GetDeviceClass();
	fName = bDevice->GetFriendlyName();
	// AKAIR rssi we can just have it @ inquiry time...
}


DeviceListItem::~DeviceListItem()
{

}


void
DeviceListItem::DrawItem(BView* owner, BRect itemRect, bool complete)
{
	rgb_color textColor = ui_color(B_LIST_ITEM_TEXT_COLOR);
	rgb_color selectedBg = ui_color(B_LIST_SELECTED_BACKGROUND_COLOR);
	rgb_color selectedText = ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR);
	float inset = be_control_look->DefaultLabelSpacing();

	if (IsSelected() || complete) {
		rgb_color color;
		if (IsSelected())
			color = selectedBg;
		else
			color = owner->ViewColor();

		owner->SetHighColor(color);
		owner->SetLowColor(color);
		owner->FillRect(itemRect);
		owner->SetHighColor(IsSelected() ? selectedText : textColor);
	} else {
		owner->SetLowColor(owner->ViewColor());
		owner->SetHighColor(textColor);
	}

	font_height finfo;
	be_plain_font->GetHeight(&finfo);
	float lineHeight = finfo.ascent + finfo.descent + finfo.leading;
	float textLeft = itemRect.left + DeviceClass::PixelsForIcon + 2 * inset;

	// Vertically center 2 lines of text
	float lineGap = inset / 2;
	float totalTextHeight = 2 * lineHeight + lineGap;
	float textTop = itemRect.top
		+ (itemRect.Height() - totalTextHeight) / 2;

	rgb_color baseColor = IsSelected() ? selectedText : textColor;

	// Line 1 (top): name in bold
	BPoint point(textLeft, textTop + finfo.ascent);
	owner->SetFont(be_bold_font);
	owner->MovePenTo(point);
	owner->DrawString(fName.String());

	// Line 2 (bottom): device class in lighter color
	point.y += lineHeight + lineGap;
	owner->SetFont(be_plain_font);
	BString classLine;
	fClass.GetMajorDeviceClass(classLine);
	classLine << " / ";
	fClass.GetMinorDeviceClass(classLine);
	owner->SetHighColor(tint_color(baseColor, 0.7));
	owner->MovePenTo(point);
	owner->DrawString(classLine.String());
	owner->SetHighColor(baseColor);

	// Icon (vertically centered)
	float iconSize = DeviceClass::PixelsForIcon + 2 * DeviceClass::IconInsets;
	float iconTop = itemRect.top
		+ (itemRect.Height() - iconSize) / 2;
	fClass.Draw(owner, BPoint(itemRect.left, iconTop));
}


void
DeviceListItem::Update(BView* owner, const BFont* font)
{
	BListItem::Update(owner, font);

	font_height height;
	font->GetHeight(&height);
	float inset = be_control_look->DefaultLabelSpacing();
	float lineGap = inset / 2;
	SetHeight(MAX((height.ascent + height.descent + height.leading) * TEXT_ROWS
		+ lineGap + 2 * inset,
		DeviceClass::PixelsForIcon + 2 * DeviceClass::IconInsets + inset));
}


int
DeviceListItem::Compare(const void	*firstArg, const void	*secondArg)
{
	const DeviceListItem* item1 = *static_cast<const DeviceListItem* const *>
		(firstArg);
	const DeviceListItem* item2 = *static_cast<const DeviceListItem* const *>
		(secondArg);

	return (int)bdaddrUtils::Compare(item1->fAddress, item2->fAddress);
}


BluetoothDevice*
DeviceListItem::Device() const
{
	return fDevice;
}


}
