/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * PbapBrowserWindow — GUI for browsing phone contacts via PBAP.
 */

#include "PbapBrowserWindow.h"

#include <Catalog.h>
#include <ControlLook.h>
#include <Entry.h>
#include <File.h>
#include <LayoutBuilder.h>
#include <MenuItem.h>
#include <Messenger.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <SeparatorView.h>

#include <bluetooth/PbapClient.h>
#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/obex.h>

#include <stdlib.h>
#include <string.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "PBAP Browser"

static const uint32 kMsgPbapDownload  = 'pbDl';
static const uint32 kMsgPbapProgress  = 'pbPr';
static const uint32 kMsgPbapDone      = 'pbDn';
static const uint32 kMsgPbapFailed    = 'pbFa';
static const uint32 kMsgPbapSave      = 'pbSv';
static const uint32 kMsgPbapSaveDone  = 'pbSd';
static const uint32 kMsgPbapClose     = 'pbCl';
static const uint32 kMsgPbapFolderSel = 'pbFs';

struct PhonebookEntry {
	const char* label;
	const char* path;
	const char* folder;
};

static const PhonebookEntry kPhonebooks[] = {
	{ B_TRANSLATE_MARK("Phonebook"),      PBAP_PATH_PHONEBOOK,       "pb"  },
	{ B_TRANSLATE_MARK("Incoming calls"), PBAP_PATH_INCOMING_CALLS,  "ich" },
	{ B_TRANSLATE_MARK("Outgoing calls"), PBAP_PATH_OUTGOING_CALLS,  "och" },
	{ B_TRANSLATE_MARK("Missed calls"),   PBAP_PATH_MISSED_CALLS,    "mch" },
	{ B_TRANSLATE_MARK("Combined calls"), PBAP_PATH_COMBINED_CALLS,  "cch" },
};

static const int kPhonebookCount
	= sizeof(kPhonebooks) / sizeof(kPhonebooks[0]);


// #pragma mark - ContactItem


class ContactItem : public BListItem {
public:
	ContactItem(const char* name, const char* phone)
		: BListItem(), fName(name), fPhone(phone) {}

	const BString& Name() const { return fName; }
	const BString& Phone() const { return fPhone; }

	void DrawItem(BView* owner, BRect frame, bool complete)
	{
		rgb_color textColor = ui_color(B_LIST_ITEM_TEXT_COLOR);
		rgb_color selectedText = ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR);

		if (IsSelected() || complete) {
			rgb_color color = IsSelected()
				? ui_color(B_LIST_SELECTED_BACKGROUND_COLOR)
				: owner->ViewColor();
			owner->SetHighColor(color);
			owner->SetLowColor(color);
			owner->FillRect(frame);
		} else {
			owner->SetLowColor(owner->ViewColor());
		}

		rgb_color base = IsSelected() ? selectedText : textColor;
		font_height fh;
		be_plain_font->GetHeight(&fh);
		float lineHeight = fh.ascent + fh.descent + fh.leading;
		float inset = be_control_look->DefaultLabelSpacing();

		// Line 1: name (bold)
		BPoint point(frame.left + inset,
			frame.top + fh.ascent + inset / 2);
		owner->SetFont(be_bold_font);
		owner->SetHighColor(base);
		owner->MovePenTo(point);
		owner->DrawString(fName.Length() > 0
			? fName.String() : "(no name)");

		// Line 2: phone (plain, dimmer)
		point.y += lineHeight + 2;
		owner->SetFont(be_plain_font);
		owner->SetHighColor(tint_color(base, 0.7));
		owner->MovePenTo(point);
		owner->DrawString(fPhone.Length() > 0
			? fPhone.String() : "(no number)");
		owner->SetHighColor(base);
	}

	void Update(BView* owner, const BFont* font)
	{
		BListItem::Update(owner, font);
		font_height fh;
		font->GetHeight(&fh);
		float lineHeight = fh.ascent + fh.descent + fh.leading;
		float inset = be_control_look->DefaultLabelSpacing();
		SetHeight(lineHeight * 2 + inset + 2);
	}

private:
	BString fName;
	BString fPhone;
};


// #pragma mark - PbapBrowserWindow


PbapBrowserWindow::PbapBrowserWindow(const bdaddr_t& address,
	const char* deviceName)
	:
	BWindow(BRect(200, 200, 650, 500),
		B_TRANSLATE("Phonebook"),
		B_TITLED_WINDOW,
		B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS),
	fDeviceName(deviceName),
	fCancelRequested(false),
	fDownloadThread(-1),
	fSavePanel(NULL),
	fVcardData(NULL),
	fVcardLen(0)
{
	bdaddrUtils::Copy(fAddress, address);

	BString title;
	title.SetToFormat(B_TRANSLATE("Phonebook: %s"), deviceName);
	SetTitle(title.String());

	fSelectedPath = PBAP_PATH_PHONEBOOK;
	fSelectedFolder = "pb";

	fStatusView = new BStringView("status", B_TRANSLATE("Ready"));

	fContactList = new BListView("contacts");
	fContactScroll = new BScrollView("scroll", fContactList,
		B_WILL_DRAW, false, true);

	// Folder selector
	BPopUpMenu* folderPopup = new BPopUpMenu("folder");
	for (int i = 0; i < kPhonebookCount; i++) {
		BMessage* msg = new BMessage(kMsgPbapFolderSel);
		msg->AddInt32("index", i);
		BMenuItem* item = new BMenuItem(
			B_TRANSLATE_NOCOLLECT(kPhonebooks[i].label), msg);
		if (i == 0)
			item->SetMarked(true);
		folderPopup->AddItem(item);
	}
	fFolderMenu = new BMenuField("folder_menu",
		B_TRANSLATE("Source:"), folderPopup);

	fDownloadButton = new BButton("download",
		B_TRANSLATE("Download"),
		new BMessage(kMsgPbapDownload));
	fSaveButton = new BButton("save",
		B_TRANSLATE("Save" B_UTF8_ELLIPSIS),
		new BMessage(kMsgPbapSave));
	fSaveButton->SetEnabled(false);
	fCloseButton = new BButton("close",
		B_TRANSLATE("Close"),
		new BMessage(kMsgPbapClose));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(fStatusView)
		.Add(fFolderMenu)
		.Add(fContactScroll, 5.0f)
		.Add(new BSeparatorView(B_HORIZONTAL))
		.AddGroup(B_HORIZONTAL)
			.Add(fDownloadButton)
			.Add(fSaveButton)
			.AddGlue()
			.Add(fCloseButton)
		.End()
	.End();
}


PbapBrowserWindow::~PbapBrowserWindow()
{
	fCancelRequested = true;
	if (fDownloadThread >= 0) {
		status_t exitVal;
		wait_for_thread(fDownloadThread, &exitVal);
	}
	delete fSavePanel;
	free(fVcardData);
	_ClearContacts();
}


void
PbapBrowserWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgPbapFolderSel:
		{
			int32 index = 0;
			if (message->FindInt32("index", &index) == B_OK
				&& index >= 0 && index < kPhonebookCount) {
				fSelectedPath = kPhonebooks[index].path;
				fSelectedFolder = kPhonebooks[index].folder;
			}
			break;
		}

		case kMsgPbapDownload:
		{
			if (fDownloadThread >= 0)
				break;
			fCancelRequested = false;
			fDownloadButton->SetEnabled(false);
			fSaveButton->SetEnabled(false);
			_ClearContacts();
			free(fVcardData);
			fVcardData = NULL;
			fVcardLen = 0;

			fDownloadThread = spawn_thread(_DownloadThreadEntry,
				"pbap_download", B_NORMAL_PRIORITY, this);
			if (fDownloadThread >= 0)
				resume_thread(fDownloadThread);
			break;
		}

		case kMsgPbapProgress:
		{
			const char* text = NULL;
			if (message->FindString("text", &text) == B_OK)
				fStatusView->SetText(text);
			break;
		}

		case kMsgPbapDone:
		{
			fDownloadThread = -1;
			fDownloadButton->SetEnabled(true);
			if (fVcardData != NULL && fVcardLen > 0) {
				_ParseVCards(fVcardData, fVcardLen);
				BString status;
				status.SetToFormat(
					B_TRANSLATE("Downloaded %d contacts"),
					(int)fContactList->CountItems());
				fStatusView->SetText(status.String());
				fSaveButton->SetEnabled(fVcardLen > 0);
			} else {
				fStatusView->SetText(
					B_TRANSLATE("No contacts received"));
			}
			break;
		}

		case kMsgPbapFailed:
		{
			fDownloadThread = -1;
			fDownloadButton->SetEnabled(true);
			const char* text = NULL;
			if (message->FindString("text", &text) == B_OK)
				fStatusView->SetText(text);
			else
				fStatusView->SetText(B_TRANSLATE("Download failed"));
			break;
		}

		case kMsgPbapSave:
		{
			if (fSavePanel == NULL) {
				fSavePanel = new BFilePanel(B_SAVE_PANEL, NULL, NULL,
					B_FILE_NODE, false, NULL, NULL, true, true);
				fSavePanel->SetTarget(BMessenger(this));
				fSavePanel->SetMessage(
					new BMessage(kMsgPbapSaveDone));
				fSavePanel->SetSaveText("contacts.vcf");
			}
			fSavePanel->Show();
			break;
		}

		case kMsgPbapSaveDone:
		{
			entry_ref dir;
			BString name;
			if (message->FindRef("directory", &dir) == B_OK
				&& message->FindString("name", &name) == B_OK
				&& fVcardData != NULL && fVcardLen > 0) {
				BPath path(&dir);
				path.Append(name.String());
				BFile file(path.Path(),
					B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
				if (file.InitCheck() == B_OK) {
					file.Write(fVcardData, fVcardLen);
					BString status;
					status.SetToFormat(
						B_TRANSLATE("Saved to %s"), path.Leaf());
					fStatusView->SetText(status.String());
				} else {
					fStatusView->SetText(
						B_TRANSLATE("Failed to save file"));
				}
			}
			break;
		}

		case kMsgPbapClose:
			PostMessage(B_QUIT_REQUESTED);
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


/*static*/ int32
PbapBrowserWindow::_DownloadThreadEntry(void* arg)
{
	static_cast<PbapBrowserWindow*>(arg)->_DownloadThread();
	return 0;
}


void
PbapBrowserWindow::_DownloadThread()
{
	BMessage progress(kMsgPbapProgress);
	progress.AddString("text",
		B_TRANSLATE("Connecting" B_UTF8_ELLIPSIS));
	PostMessage(&progress);

	Bluetooth::PbapClient pbap;
	status_t err = pbap.ConnectL2cap(fAddress);
	if (err != B_OK)
		err = pbap.Connect(fAddress);

	if (err != B_OK || fCancelRequested) {
		BMessage fail(kMsgPbapFailed);
		BString errText;
		errText.SetToFormat(B_TRANSLATE("Connection failed: %s"),
			strerror(err));
		fail.AddString("text", errText.String());
		PostMessage(&fail);
		return;
	}

	// Navigate: root -> telecom -> subfolder
	progress.MakeEmpty();
	progress.what = kMsgPbapProgress;
	progress.AddString("text",
		B_TRANSLATE("Navigating folders" B_UTF8_ELLIPSIS));
	PostMessage(&progress);

	err = pbap.SetPath(NULL);
	if (err == B_OK)
		err = pbap.SetPath("telecom");
	if (err == B_OK)
		err = pbap.SetPath(fSelectedFolder.String());

	if (err != B_OK || fCancelRequested) {
		BMessage fail(kMsgPbapFailed);
		BString errText;
		errText.SetToFormat(B_TRANSLATE("Navigation failed: %s"),
			strerror(err));
		fail.AddString("text", errText.String());
		pbap.Disconnect();
		PostMessage(&fail);
		return;
	}

	// Show approval message — phone may prompt user
	progress.MakeEmpty();
	progress.what = kMsgPbapProgress;
	progress.AddString("text",
		B_TRANSLATE("Downloading — approve contact sharing on phone"
			B_UTF8_ELLIPSIS));
	PostMessage(&progress);

	uint8* data = NULL;
	size_t dataLen = 0;
	err = pbap.PullPhoneBook(fSelectedPath.String(),
		PBAP_FORMAT_VCARD_21, &data, &dataLen);

	pbap.Disconnect();

	if (err != B_OK || fCancelRequested) {
		free(data);
		BMessage fail(kMsgPbapFailed);
		BString errText;
		errText.SetToFormat(B_TRANSLATE("Download failed: %s"),
			strerror(err));
		fail.AddString("text", errText.String());
		PostMessage(&fail);
		return;
	}

	free(fVcardData);
	fVcardData = data;
	fVcardLen = dataLen;

	PostMessage(new BMessage(kMsgPbapDone));
}


void
PbapBrowserWindow::_ParseVCards(const uint8* data, size_t len)
{
	if (data == NULL || len == 0)
		return;

	BString name;
	BString phone;
	bool inCard = false;

	const char* pos = (const char*)data;
	const char* end = pos + len;

	while (pos < end) {
		const char* eol = pos;
		while (eol < end && *eol != '\n' && *eol != '\r')
			eol++;

		int32 lineLen = eol - pos;
		BString line(pos, lineLen);

		if (line.ICompare("BEGIN:VCARD") == 0) {
			inCard = true;
			name = "";
			phone = "";
		} else if (line.ICompare("END:VCARD") == 0) {
			if (inCard && (name.Length() > 0 || phone.Length() > 0))
				fContactList->AddItem(
					new ContactItem(name.String(), phone.String()));
			inCard = false;
		} else if (inCard) {
			if (line.IFindFirst("FN:") == 0
				|| line.IFindFirst("FN;") == 0) {
				int32 colon = line.FindFirst(':');
				if (colon >= 0)
					name.SetTo(line.String() + colon + 1);
			} else if (name.Length() == 0
				&& (line.IFindFirst("N:") == 0
					|| line.IFindFirst("N;") == 0)) {
				// Fallback: use N field if FN not yet found
				int32 colon = line.FindFirst(':');
				if (colon >= 0) {
					BString raw(line.String() + colon + 1);
					raw.ReplaceAll(';', ' ');
					if (raw.Length() > 0)
						name = raw;
				}
			} else if (phone.Length() == 0
				&& (line.IFindFirst("TEL:") == 0
					|| line.IFindFirst("TEL;") == 0)) {
				int32 colon = line.FindFirst(':');
				if (colon >= 0)
					phone.SetTo(line.String() + colon + 1);
			}
		}

		// Skip line ending
		pos = eol;
		if (pos < end && *pos == '\r')
			pos++;
		if (pos < end && *pos == '\n')
			pos++;
	}
}


void
PbapBrowserWindow::_ClearContacts()
{
	for (int32 i = fContactList->CountItems() - 1; i >= 0; i--)
		delete fContactList->RemoveItem(i);
}
