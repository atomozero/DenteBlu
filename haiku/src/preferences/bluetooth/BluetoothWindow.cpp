/*
 * Copyright 2008-10, Oliver Ruiz Dorantes, <oliver.ruiz.dorantes_at_gmail.com>
 * Copyright 2025, Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include "BluetoothWindow.h"

#include "BluetoothDeviceView.h"
#include "BluetoothSettingsView.h"
#include "DeviceListItem.h"
#include "SdpServicesWindow.h"
#include "SppTerminalWindow.h"
#include "OppTransferWindow.h"
#include "PbapBrowserWindow.h"
#include "HfpCallWindow.h"
#include "A2dpPlayerWindow.h"

#include <Button.h>
#include <Catalog.h>
#include <ControlLook.h>
#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <Roster.h>
#include <SeparatorView.h>
#include <StatusBar.h>

#include <stdio.h>

#include <bluetooth/DiscoveryAgent.h>
#include <bluetooth/DiscoveryListener.h>
#include <bluetooth/LocalDevice.h>
#include <bluetooth/RemoteDevice.h>
#include <bluetooth/bdaddrUtils.h>

#include "defs.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Window"

static const uint32 kMsgSetDefaults = 'dflt';
static const uint32 kMsgRevert = 'rvrt';

static const uint32 kMsgStartServices = 'SrSR';
static const uint32 kMsgStopServices = 'StST';

static const uint32 kMsgInquiryStart = 'iStR';
static const uint32 kMsgInquiryFinish = 'iFin';
static const uint32 kMsgInquiryDeviceFound = 'iDvF';
static const uint32 kMsgInquirySecond = 'iSec';
static const uint32 kMsgInquiryRetrieve = 'iRet';

// private functionality provided by kit
extern uint8 GetInquiryTime();

LocalDevice* ActiveLocalDevice = NULL;


// #pragma mark - TitleItem


class TitleItem : public BStringItem {
public:
	TitleItem(const char* label)
		: BStringItem(label) {}

	void DrawItem(BView* owner, BRect frame, bool complete) {
		owner->SetFont(be_bold_font);
		BStringItem::DrawItem(owner, frame, complete);
		owner->SetFont(be_plain_font);
	}

	void Update(BView* owner, const BFont* font) {
		BStringItem::Update(owner, be_bold_font);
	}
};


// #pragma mark - AdapterListItem


class AdapterListItem : public BStringItem {
public:
	AdapterListItem(LocalDevice* device)
		: BStringItem(device->GetFriendlyName().String()),
		  fDevice(device) {}

	LocalDevice* Device() const { return fDevice; }

private:
	LocalDevice* fDevice;
};


// #pragma mark - PairedDeviceItem


class PairedDeviceItem : public BListItem {
public:
	PairedDeviceItem(const BString& address, const BString& name,
			uint32 cod)
		:
		BListItem(),
		fAddress(address),
		fName(name),
		fClass()
	{
		if (cod != 0) {
			uint8 record[3];
			record[0] = cod & 0xFF;
			record[1] = (cod >> 8) & 0xFF;
			record[2] = (cod >> 16) & 0xFF;
			fClass.SetRecord(record);
		}
	}

	const BString& Address() const { return fAddress; }
	const BString& Name() const { return fName; }

	void DrawItem(BView* owner, BRect itemRect, bool complete)
	{
		rgb_color textColor = ui_color(B_LIST_ITEM_TEXT_COLOR);
		rgb_color selectedBg = ui_color(B_LIST_SELECTED_BACKGROUND_COLOR);
		rgb_color selectedText = ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR);

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
		float inset = be_control_look->DefaultLabelSpacing();
		float lineHeight = finfo.ascent + finfo.descent + finfo.leading;
		float textLeft = itemRect.left + DeviceClass::PixelsForIcon
			+ 2 * inset;
		rgb_color baseColor = IsSelected() ? selectedText : textColor;

		// Vertically center 3 lines of text
		float lineGap = inset / 2;
		float totalTextHeight = 3 * lineHeight + 2 * lineGap;
		float textTop = itemRect.top
			+ (itemRect.Height() - totalTextHeight) / 2;

		// Line 1 (top): name
		BPoint point(textLeft, textTop + finfo.ascent);
		owner->SetFont(be_plain_font);
		owner->MovePenTo(point);
		if (fName.Length() > 0)
			owner->DrawString(fName.String());
		else {
			owner->SetHighColor(tint_color(baseColor, 0.7));
			owner->DrawString(B_TRANSLATE("Unknown device"));
			owner->SetHighColor(baseColor);
		}

		// Line 2 (middle): address
		point.y += lineHeight + lineGap;
		owner->SetFont(be_fixed_font);
		owner->MovePenTo(point);
		owner->DrawString(fAddress.String());

		// Line 3 (bottom): device class
		point.y += lineHeight + lineGap;
		if (!fClass.IsUnknownDeviceClass()) {
			BString classLine;
			fClass.GetMajorDeviceClass(classLine);
			classLine << " / ";
			fClass.GetMinorDeviceClass(classLine);
			owner->SetFont(be_plain_font);
			owner->SetHighColor(tint_color(baseColor, 0.7));
			owner->MovePenTo(point);
			owner->DrawString(classLine.String());
			owner->SetHighColor(baseColor);
		}

		// Icon (vertically centered)
		float iconSize = DeviceClass::PixelsForIcon
			+ 2 * DeviceClass::IconInsets;
		float iconTop = itemRect.top
			+ (itemRect.Height() - iconSize) / 2;
		fClass.Draw(owner, BPoint(itemRect.left, iconTop));
	}

	void Update(BView* owner, const BFont* font)
	{
		BListItem::Update(owner, font);
		font_height height;
		font->GetHeight(&height);
		float inset = be_control_look->DefaultLabelSpacing();
		float lineGap = inset / 2;
		SetHeight(MAX(
			(height.ascent + height.descent + height.leading) * 3
				+ 2 * lineGap + 2 * inset,
			DeviceClass::PixelsForIcon + 2 * DeviceClass::IconInsets
				+ inset));
	}

private:
	BString		fAddress;
	BString		fName;
	DeviceClass	fClass;
};


// #pragma mark - SidebarDiscoveryListener


using Bluetooth::DeviceListItem;


class SidebarDiscoveryListener : public DiscoveryListener {
public:
	SidebarDiscoveryListener(BluetoothWindow* window)
		:
		DiscoveryListener(),
		fWindow(window)
	{
	}

	void
	DeviceDiscovered(RemoteDevice* btDevice, DeviceClass cod)
	{
		BMessage* message = new BMessage(kMsgInquiryDeviceFound);
		message->AddPointer("deviceItem", new DeviceListItem(btDevice));
		fWindow->PostMessage(message);
	}

	void
	InquiryCompleted(int discType)
	{
		fWindow->PostMessage(new BMessage(kMsgInquiryFinish));
	}

	void
	InquiryStarted(status_t status)
	{
		fWindow->PostMessage(new BMessage(kMsgInquiryStart));
	}

private:
	BluetoothWindow*	fWindow;
};


// #pragma mark - BluetoothWindow


BluetoothWindow::BluetoothWindow(BRect frame)
	:
	BWindow(frame, B_TRANSLATE_SYSTEM_NAME("Bluetooth"), B_TITLED_WINDOW,
		B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS),
	fDiscoveryAgent(NULL),
	fScanning(false),
	fRetrieving(false),
	fRetrievalIndex(0),
	fRetrievalLabelPlaced(false),
	fScanTimer(0),
	fScanElapsed(0),
	fSettingsView(NULL),
	fDeviceDetailView(NULL),
	fDeviceInfoView(NULL)
{
	// Bottom buttons
	fDefaultsButton = new BButton("defaults", B_TRANSLATE("Defaults"),
		new BMessage(kMsgSetDefaults), B_WILL_DRAW);
	fDefaultsButton->SetEnabled(false);

	fRevertButton = new BButton("revert", B_TRANSLATE("Revert"),
		new BMessage(kMsgRevert), B_WILL_DRAW);
	fRevertButton->SetEnabled(false);

	// Menu bar
	fMenubar = new BMenuBar("menu_bar");

	BMenu* menu = new BMenu(B_TRANSLATE("Server"));
	menu->AddItem(new BMenuItem(
		B_TRANSLATE("Start bluetooth services" B_UTF8_ELLIPSIS),
		new BMessage(kMsgStartServices), 0));
	menu->AddItem(new BMenuItem(
		B_TRANSLATE("Stop bluetooth services" B_UTF8_ELLIPSIS),
		new BMessage(kMsgStopServices), 0));
	menu->AddSeparatorItem();
	menu->AddItem(new BMenuItem(
		B_TRANSLATE("Refresh local devices" B_UTF8_ELLIPSIS),
		new BMessage(kMsgRefresh), 0));
	fMenubar->AddItem(menu);

	menu = new BMenu(B_TRANSLATE("Help"));
	menu->AddItem(new BMenuItem(B_TRANSLATE("About Bluetooth" B_UTF8_ELLIPSIS),
		new BMessage(B_ABOUT_REQUESTED), 0));
	fMenubar->AddItem(menu);

	// Sidebar
	fListView = new BOutlineListView("list", B_SINGLE_SELECTION_LIST);
	fListView->SetSelectionMessage(new BMessage(kMsgItemSelected));

	fScrollView = new BScrollView("scroll", fListView, 0, false, true);
	fScrollView->SetExplicitMinSize(BSize(220, B_SIZE_UNSET));


	// Discovery
	fDiscoveryListener = new SidebarDiscoveryListener(this);
	fSecondsMessage = new BMessage(kMsgInquirySecond);
	fRetrieveMessage = new BMessage(kMsgInquiryRetrieve);

	// Scan progress bar
	fScanProgress = new BStatusBar("scanProgress");
	fScanProgress->SetBarHeight(8.0f);
	fScanActiveColor = fScanProgress->BarColor();
	fScanProgress->SetBarColor(ui_color(B_PANEL_BACKGROUND_COLOR));

	// Detail container
	fDetailView = new BView("detail", 0);
	fDetailView->SetLayout(new BGroupLayout(B_VERTICAL));
	fDetailView->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	// Adapter detail (settings view, created once)
	fSettingsView = new BluetoothSettingsView(B_TRANSLATE("Settings"));

	// Device detail view (created once, switched in/out)
	fDeviceInfoView = new BluetoothDeviceView(NULL);
	fPairButton = new BButton("pair", B_TRANSLATE("Pair" B_UTF8_ELLIPSIS),
		new BMessage(kMsgPairDevice));
	fDisconnectButton = new BButton("disconnect",
		B_TRANSLATE("Disconnect"), new BMessage(kMsgDisconnectDevice));
	fServicesButton = new BButton("services",
		B_TRANSLATE("Services" B_UTF8_ELLIPSIS),
		new BMessage(kMsgQueryServices));
	fTerminalButton = new BButton("terminal",
		B_TRANSLATE("Terminal" B_UTF8_ELLIPSIS),
		new BMessage(kMsgTerminal));
	fFileTransferButton = new BButton("filetransfer",
		B_TRANSLATE("Send File" B_UTF8_ELLIPSIS),
		new BMessage(kMsgFileTransfer));
	fPbapButton = new BButton("pbap",
		B_TRANSLATE("Contacts" B_UTF8_ELLIPSIS),
		new BMessage(kMsgPbapBrowser));
	fCallButton = new BButton("call",
		B_TRANSLATE("Call" B_UTF8_ELLIPSIS),
		new BMessage(kMsgCallDevice));
	fA2dpButton = new BButton("a2dp",
		B_TRANSLATE("Music" B_UTF8_ELLIPSIS),
		new BMessage(kMsgA2dpPlayer));

	fDeviceInfoView->SetExplicitAlignment(
		BAlignment(B_ALIGN_HORIZONTAL_UNSET, B_ALIGN_TOP));

	fDeviceDetailView = new BView("deviceDetail", 0);
	BLayoutBuilder::Group<>(fDeviceDetailView, B_VERTICAL, B_USE_HALF_ITEM_SPACING)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.Add(fDeviceInfoView)
		.Add(new BSeparatorView(B_HORIZONTAL))
		.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
			.Add(fPairButton)
			.Add(fDisconnectButton)
			.Add(fServicesButton)
			.AddGlue()
		.End()
		.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
			.Add(fTerminalButton)
			.Add(fFileTransferButton)
			.Add(fPbapButton)
			.Add(fCallButton)
			.Add(fA2dpButton)
			.AddGlue()
		.End()
		.AddGlue()
	.End();

	// Main layout
	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.SetInsets(0)
		.Add(fMenubar)
		.AddStrut(B_USE_HALF_ITEM_SPACING)
		.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
			.SetInsets(B_USE_WINDOW_SPACING, 0,
				B_USE_WINDOW_SPACING, 0)
			.AddGroup(B_VERTICAL, 0)
				.Add(fScrollView, 4.0f)
				.Add(fScanProgress, 0)
			.End()
			.Add(fDetailView, 6.0f)
		.End()
		.AddStrut(B_USE_HALF_ITEM_SPACING)
		.Add(new BSeparatorView(B_HORIZONTAL))
		.AddGroup(B_HORIZONTAL)
			.SetInsets(B_USE_WINDOW_SPACING, B_USE_DEFAULT_SPACING,
				B_USE_WINDOW_SPACING, B_USE_WINDOW_SPACING)
			.Add(fDefaultsButton)
			.Add(fRevertButton)
			.AddGlue()
		.End()
	.End();

	_BuildSidebar();

	// Auto-start device scan
	PostMessage(new BMessage(kMsgAddDevices));
}


void
BluetoothWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgItemSelected:
		{
			int32 index = fListView->CurrentSelection();
			if (index < 0)
				break;
			BListItem* item = fListView->ItemAt(index);
			_SelectItem(item);
			break;
		}

		case kMsgAddDevices:
		{
			if (fScanning || fRetrieving)
				break;
			if (ActiveLocalDevice == NULL) {
				/* Device not ready yet — retry in 1 second */
				BMessenger messenger(this);
				BMessageRunner::StartSending(messenger,
					new BMessage(kMsgAddDevices), 1000000, 1);
				break;
			}

			// Start inquiry (paired devices remain in sidebar)
			fDiscoveryAgent = ActiveLocalDevice->GetDiscoveryAgent();
			fDiscoveryAgent->StartInquiry(BT_GIAC, fDiscoveryListener,
				GetInquiryTime());
			break;
		}

		case kMsgPairDevice:
		{
			int32 index = fListView->CurrentSelection();
			if (index < 0)
				break;
			Bluetooth::DeviceListItem* devItem
				= dynamic_cast<Bluetooth::DeviceListItem*>(
					fListView->ItemAt(index));
			if (devItem == NULL)
				break;
			RemoteDevice* remote
				= dynamic_cast<RemoteDevice*>(devItem->Device());
			if (remote != NULL)
				remote->Authenticate();
			break;
		}

		case kMsgDisconnectDevice:
		{
			int32 index = fListView->CurrentSelection();
			if (index < 0)
				break;
			Bluetooth::DeviceListItem* devItem
				= dynamic_cast<Bluetooth::DeviceListItem*>(
					fListView->ItemAt(index));
			if (devItem == NULL)
				break;
			RemoteDevice* remote
				= dynamic_cast<RemoteDevice*>(devItem->Device());
			if (remote != NULL)
				remote->Disconnect();
			break;
		}

		case kMsgQueryServices:
		{
			int32 index = fListView->CurrentSelection();
			if (index < 0)
				break;

			bdaddr_t addr;
			BString name;

			// Try scanned device first, then paired device
			Bluetooth::DeviceListItem* devItem
				= dynamic_cast<Bluetooth::DeviceListItem*>(
					fListView->ItemAt(index));
			if (devItem != NULL && devItem->Device() != NULL) {
				addr = devItem->Device()->GetBluetoothAddress();
				name = devItem->Device()->GetFriendlyName();
			} else {
				PairedDeviceItem* paired
					= dynamic_cast<PairedDeviceItem*>(
						fListView->ItemAt(index));
				if (paired == NULL)
					break;
				addr = bdaddrUtils::FromString(
					paired->Address().String());
				name = paired->Name();
			}

			SdpServicesWindow* window
				= new SdpServicesWindow(addr, name.String());
			window->Show();
			break;
		}

		case kMsgTerminal:
		{
			int32 index = fListView->CurrentSelection();
			if (index < 0)
				break;
			PairedDeviceItem* paired
				= dynamic_cast<PairedDeviceItem*>(fListView->ItemAt(index));
			if (paired != NULL) {
				bdaddr_t addr = bdaddrUtils::FromString(
					paired->Address().String());
				(new SppTerminalWindow(addr,
					paired->Name().String()))->Show();
			}
			break;
		}

		case kMsgFileTransfer:
		{
			int32 index = fListView->CurrentSelection();
			if (index < 0)
				break;
			PairedDeviceItem* paired
				= dynamic_cast<PairedDeviceItem*>(fListView->ItemAt(index));
			if (paired != NULL) {
				bdaddr_t addr = bdaddrUtils::FromString(
					paired->Address().String());
				(new OppTransferWindow(addr,
					paired->Name().String()))->Show();
			}
			break;
		}

		case kMsgPbapBrowser:
		{
			int32 index = fListView->CurrentSelection();
			if (index < 0)
				break;
			PairedDeviceItem* paired
				= dynamic_cast<PairedDeviceItem*>(fListView->ItemAt(index));
			if (paired != NULL) {
				bdaddr_t addr = bdaddrUtils::FromString(
					paired->Address().String());
				(new PbapBrowserWindow(addr,
					paired->Name().String()))->Show();
			}
			break;
		}

		case kMsgCallDevice:
		{
			int32 index = fListView->CurrentSelection();
			if (index < 0)
				break;
			PairedDeviceItem* paired
				= dynamic_cast<PairedDeviceItem*>(fListView->ItemAt(index));
			if (paired != NULL) {
				bdaddr_t addr = bdaddrUtils::FromString(
					paired->Address().String());
				(new HfpCallWindow(addr,
					paired->Name().String()))->Show();
			}
			break;
		}

		case kMsgA2dpPlayer:
		{
			int32 index = fListView->CurrentSelection();
			if (index < 0)
				break;
			PairedDeviceItem* paired
				= dynamic_cast<PairedDeviceItem*>(fListView->ItemAt(index));
			if (paired != NULL) {
				bdaddr_t addr = bdaddrUtils::FromString(
					paired->Address().String());
				(new A2dpPlayerWindow(addr,
					paired->Name().String()))->Show();
			}
			break;
		}

		case kMsgAddToRemoteList:
		{
			BListItem* device;
			if (message->FindPointer("device", (void**)&device) == B_OK) {
				fListView->AddUnder(device, fDevicesTitle);
				fListView->Invalidate();
			}
			break;
		}

		case kMsgInquiryStart:
		{
			fScanning = true;
			fScanElapsed = 0;
			fScanTimer = BT_BASE_INQUIRY_TIME * GetInquiryTime() + 1;

			fScanProgress->Reset();
			fScanProgress->SetMaxValue(fScanTimer);
			fScanProgress->SetTo(1);
			fScanProgress->SetBarColor(fScanActiveColor);
			fScanProgress->SetTrailingText(
				B_TRANSLATE("Starting scan" B_UTF8_ELLIPSIS));

			BMessenger messenger(this);
			BMessageRunner::StartSending(messenger, fSecondsMessage,
				1000000, (int32)fScanTimer);

			fScanElapsed = 1;
			break;
		}

		case kMsgInquiryDeviceFound:
		{
			DeviceListItem* listItem;
			if (message->FindPointer("deviceItem",
					(void**)&listItem) == B_OK) {
				BString newAddr = bdaddrUtils::ToString(
					listItem->Device()->GetBluetoothAddress());
				int32 count = fListView->CountItemsUnder(
					fDevicesTitle, false);
				bool duplicate = false;
				for (int32 i = 0; i < count; i++) {
					BListItem* under = fListView->ItemUnderAt(
						fDevicesTitle, false, i);

					// Replace PairedDeviceItem with live one
					PairedDeviceItem* paired
						= dynamic_cast<PairedDeviceItem*>(under);
					if (paired != NULL
						&& paired->Address() == newAddr) {
						fListView->RemoveItem(under);
						delete under;
						break;
					}

					// Skip if already discovered in this scan
					DeviceListItem* existing
						= dynamic_cast<DeviceListItem*>(under);
					if (existing != NULL) {
						BString existAddr = bdaddrUtils::ToString(
							existing->Device()
								->GetBluetoothAddress());
						if (existAddr == newAddr) {
							delete listItem;
							duplicate = true;
							break;
						}
					}
				}
				if (!duplicate) {
					fListView->AddUnder(listItem, fDevicesTitle);
					fListView->Invalidate();
				}
			}
			break;
		}

		case kMsgInquirySecond:
		{
			if (fScanning && fScanElapsed < fScanTimer) {
				fScanProgress->SetTo(fScanElapsed * 100 / fScanTimer);

				BString elapsedTime = B_TRANSLATE("Remaining %1 seconds");
				BString seconds;
				seconds << (int)(fScanTimer - fScanElapsed);
				elapsedTime.ReplaceFirst("%1", seconds.String());
				fScanProgress->SetTrailingText(elapsedTime.String());

				fScanElapsed += 1;
			}
			break;
		}

		case kMsgInquiryFinish:
		{
			fScanning = false;
			fRetrieving = true;
			fRetrievalIndex = 0;
			fRetrievalLabelPlaced = false;

			fScanProgress->SetTo(100);
			fScanProgress->SetTrailingText(
				B_TRANSLATE("Retrieving names" B_UTF8_ELLIPSIS));

			BMessenger messenger(this);
			BMessageRunner::StartSending(messenger, fRetrieveMessage,
				1000000, 1);
			break;
		}

		case kMsgInquiryRetrieve:
		{
			if (!fRetrieving || fDiscoveryAgent == NULL)
				break;

			if (fRetrievalIndex
					< fDiscoveryAgent->RetrieveDevices(0).CountItems()) {
				BluetoothDevice* resolved
					= (BluetoothDevice*)fDiscoveryAgent
						->RetrieveDevices(0)
						.ItemAt(fRetrievalIndex);

				if (!fRetrievalLabelPlaced) {
					fRetrievalLabelPlaced = true;

					BString progressText(
						B_TRANSLATE("Retrieving name of %1"));
					BString namestr;
					namestr << bdaddrUtils::ToString(
						resolved->GetBluetoothAddress());
					progressText.ReplaceFirst("%1", namestr.String());
					fScanProgress->SetTrailingText(
						progressText.String());
				} else {
					// Find the matching DeviceListItem by address
					BString resolvedAddr = bdaddrUtils::ToString(
						resolved->GetBluetoothAddress());
					int32 count = fListView->CountItemsUnder(
						fDevicesTitle, false);
					for (int32 i = 0; i < count; i++) {
						BListItem* under
							= fListView->ItemUnderAt(
								fDevicesTitle, false, i);
						DeviceListItem* devItem
							= dynamic_cast<DeviceListItem*>(under);
						if (devItem == NULL)
							continue;
						BString devAddr = bdaddrUtils::ToString(
							devItem->Device()
								->GetBluetoothAddress());
						if (devAddr == resolvedAddr) {
							devItem->SetDevice(resolved);
							fListView->InvalidateItem(
								fListView->IndexOf(devItem));
							break;
						}
					}

					// Persist the resolved name and class
					BString resolvedName
						= resolved->GetFriendlyName();
					if (resolvedName.Length() > 0
						&& resolvedName[0] != '#') {
						bdaddr_t addr
							= resolved->GetBluetoothAddress();
						uint32 cod = resolved
							->GetDeviceClass().Record();
						LocalDevice::SaveDeviceName(addr,
							resolvedName.String(), cod);
					}

					fRetrievalIndex++;
					fRetrievalLabelPlaced = false;
				}

				BMessenger messenger(this);
				BMessageRunner::StartSending(messenger,
					fRetrieveMessage, 500000, 1);
			} else {
				fRetrieving = false;
				fRetrievalIndex = 0;

				fScanProgress->SetBarColor(
					ui_color(B_PANEL_BACKGROUND_COLOR));
				fScanProgress->SetTrailingText("");

				// Auto-restart scan after a brief pause
				BMessenger messenger(this);
				BMessageRunner::StartSending(messenger,
					new BMessage(kMsgAddDevices), 3000000, 1);
			}
			break;
		}

		case kMsgSetConnectionPolicy:
		case kMsgSetDeviceClass:
			fSettingsView->MessageReceived(message);
			break;

		case kMsgSetDefaults:
			break;

		case kMsgRevert:
			break;

		case kMsgStartServices:
			if (!be_roster->IsRunning(BLUETOOTH_SIGNATURE)) {
				status_t error = be_roster->Launch(BLUETOOTH_SIGNATURE);
				printf("kMsgStartServices: %s\n", strerror(error));
			}
			break;

		case kMsgStopServices:
			if (be_roster->IsRunning(BLUETOOTH_SIGNATURE)) {
				status_t error = BMessenger(BLUETOOTH_SIGNATURE)
					.SendMessage(B_QUIT_REQUESTED);
				printf("kMsgStopServices: %s\n", strerror(error));
			}
			break;

		case kMsgRefresh:
			_BuildSidebar();
			if (fSettingsView != NULL)
				fSettingsView->MessageReceived(message);
			break;

		case B_ABOUT_REQUESTED:
			be_app->PostMessage(message);
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


bool
BluetoothWindow::QuitRequested()
{
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}


void
BluetoothWindow::_BuildSidebar()
{
	// Remove all items
	while (fListView->FullListCountItems() > 0) {
		BListItem* item = fListView->FullListItemAt(0);
		fListView->RemoveItem(item);
		delete item;
	}

	_ClearDetail();

	// Adapters section
	fAdaptersTitle = new TitleItem(B_TRANSLATE("Adapters"));
	fListView->AddItem(fAdaptersTitle);

	AdapterListItem* firstAdapter = NULL;

	for (uint32 i = 0; i < LocalDevice::GetLocalDeviceCount(); i++) {
		LocalDevice* lDevice = LocalDevice::GetLocalDevice();
		if (lDevice != NULL) {
			AdapterListItem* adapterItem = new AdapterListItem(lDevice);
			fListView->AddUnder(adapterItem, fAdaptersTitle);
			if (firstAdapter == NULL)
				firstAdapter = adapterItem;
		}
	}

	// Devices section
	fDevicesTitle = new TitleItem(B_TRANSLATE("Devices"));
	fListView->AddItem(fDevicesTitle);

	// Load paired devices from key store
	BMessage pairedList;
	if (LocalDevice::GetPairedDevices(&pairedList) == B_OK) {
		int32 count = 0;
		pairedList.FindInt32("count", &count);
		for (int32 i = 0; i < count; i++) {
			BString addrStr, name;
			int32 cod = 0;
			if (pairedList.FindString("bdaddr", i, &addrStr) == B_OK) {
				pairedList.FindString("name", i, &name);
				pairedList.FindInt32("cod", i, &cod);
				fListView->AddUnder(
					new PairedDeviceItem(addrStr, name, (uint32)cod),
					fDevicesTitle);
			}
		}
	}

	fListView->Expand(fAdaptersTitle);
	fListView->Expand(fDevicesTitle);

	// Select the first adapter if available
	if (firstAdapter != NULL) {
		int32 index = fListView->IndexOf(firstAdapter);
		if (index >= 0)
			fListView->Select(index);
	}
}


void
BluetoothWindow::_SelectItem(BListItem* item)
{
	if (item == NULL)
		return;

	// Skip title items
	if (dynamic_cast<TitleItem*>(item) != NULL) {
		fListView->Deselect(fListView->IndexOf(item));
		return;
	}

	AdapterListItem* adapterItem = dynamic_cast<AdapterListItem*>(item);
	if (adapterItem != NULL) {
		_ShowAdapterDetail(adapterItem->Device());
		return;
	}

	Bluetooth::DeviceListItem* devItem
		= dynamic_cast<Bluetooth::DeviceListItem*>(item);
	if (devItem != NULL) {
		_ShowDeviceDetail(devItem);
		return;
	}

	PairedDeviceItem* pairedItem = dynamic_cast<PairedDeviceItem*>(item);
	if (pairedItem != NULL) {
		_ShowPairedDeviceDetail(pairedItem);
		return;
	}
}


void
BluetoothWindow::_ShowAdapterDetail(LocalDevice* device)
{
	_ClearDetail();
	fSettingsView->SetLocalDevice(device);
	fDetailView->AddChild(fSettingsView);
	ActiveLocalDevice = device;
}


void
BluetoothWindow::_ShowDeviceDetail(Bluetooth::DeviceListItem* item)
{
	_ClearDetail();
	if (item->Device() != NULL)
		fDeviceInfoView->SetBluetoothDevice(item->Device());
	fDetailView->AddChild(fDeviceDetailView);
}


void
BluetoothWindow::_ShowPairedDeviceDetail(PairedDeviceItem* item)
{
	_ClearDetail();

	BString nameText = item->Name();
	if (nameText.Length() == 0)
		nameText = B_TRANSLATE("Unknown device");

	fDeviceInfoView->SetDeviceInfo(nameText.String(),
		item->Address().String(),
		B_TRANSLATE("Paired device"));

	fDetailView->AddChild(fDeviceDetailView);
}


void
BluetoothWindow::_ClearDetail()
{
	while (fDetailView->CountChildren() > 0)
		fDetailView->ChildAt(0)->RemoveSelf();
}


BluetoothWindow::~BluetoothWindow()
{
	_ClearDetail();
	delete fSettingsView;
	delete fDeviceDetailView;
	if (fDiscoveryListener->Lock())
		fDiscoveryListener->Quit();
	delete fSecondsMessage;
	delete fRetrieveMessage;
}


void
BluetoothWindow::_ClearDevices()
{
	while (true) {
		BListItem* item = fListView->ItemUnderAt(fDevicesTitle, false, 0);
		if (item == NULL)
			break;
		fListView->RemoveItem(item);
		delete item;
	}
}
