/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "SettingsWindow.h"

#include <vector>

#include <Application.h>
#include <Button.h>
#include <Catalog.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <Directory.h>
#include <Entry.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <InterfaceDefs.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <StatusBar.h>
#include <StringView.h>

#include "IndexServerPrivate.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "SettingsWindow"


static const uint32 kMsgModeChanged = 'MoCh';
static const uint32 kMsgAddPath = 'AddP';
static const uint32 kMsgRemovePath = 'RemP';
static const uint32 kMsgDefaults = 'Dflt';
static const uint32 kMsgRevert = 'Rvrt';
static const uint32 kMsgToggleAnalyser = 'TgAn';
static const uint32 kMsgUpdateStatus = 'UpSt';

static const int32 kPathColumn = 0;

const bigtime_t kStatusPollInterval = 2000000; // 2s


//! The path list; also accepts folders dropped from Tracker.
class PathListView : public BColumnListView {
public:
	PathListView(const char* name)
		:
		BColumnListView(name, B_NAVIGABLE, B_PLAIN_BORDER)
	{
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->WasDropped()
			&& (message->what == B_SIMPLE_DATA
				|| message->what == B_REFS_RECEIVED)) {
			BMessage addMessage(kMsgAddPath);
			entry_ref ref;
			for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++)
				addMessage.AddRef("refs", &ref);
			if (addMessage.HasRef("refs"))
				Window()->PostMessage(&addMessage);
			return;
		}
		BColumnListView::MessageReceived(message);
	}
};


SettingsWindow::SettingsWindow()
	:
	BWindow(BRect(80, 80, 560, 420), B_TRANSLATE_SYSTEM_NAME("Index Server"),
		B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS)
{
	BMenuBar* menuBar = new BMenuBar("menu bar");
	_BuildAnalysersMenu(menuBar);

	BPopUpMenu* modeMenu = new BPopUpMenu("mode");
	BMessage* blacklistMessage = new BMessage(kMsgModeChanged);
	blacklistMessage->AddInt32("mode", kBlacklistMode);
	modeMenu->AddItem(new BMenuItem(
		B_TRANSLATE("All folders except the listed ones"),
		blacklistMessage));
	BMessage* whitelistMessage = new BMessage(kMsgModeChanged);
	whitelistMessage->AddInt32("mode", kWhitelistMode);
	modeMenu->AddItem(new BMenuItem(B_TRANSLATE("Only the listed folders"),
		whitelistMessage));
	fModeField = new BMenuField("mode", NULL, modeMenu);

	fPathListView = new PathListView("paths");
	fPathListView->AddColumn(new BStringColumn(B_TRANSLATE("Path"), 400, 100,
		2000, B_TRUNCATE_MIDDLE), kPathColumn);

	BButton* addButton = new BButton("add", B_TRANSLATE("Add" B_UTF8_ELLIPSIS),
		new BMessage(kMsgAddPath));
	BButton* removeButton = new BButton("remove", B_TRANSLATE("Remove"),
		new BMessage(kMsgRemovePath));
	BButton* defaultsButton = new BButton("defaults", B_TRANSLATE("Defaults"),
		new BMessage(kMsgDefaults));
	BButton* revertButton = new BButton("revert", B_TRANSLATE("Revert"),
		new BMessage(kMsgRevert));

	fStatusView = new BStringView("status", "");
	fStatusView->SetAlignment(B_ALIGN_LEFT);

	fProgressBar = new BStatusBar("progress");
	fProgressBar->SetMaxValue(100.0f);
	fProgressBar->Hide();

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(menuBar)
		.AddGroup(B_VERTICAL, B_USE_WINDOW_SPACING)
			.Add(fStatusView)
			.Add(fProgressBar)
			.AddGroup(B_HORIZONTAL)
				.Add(fModeField)
				.AddGlue()
				.End()
			.Add(fPathListView)
			.AddGroup(B_HORIZONTAL)
				.Add(addButton)
				.Add(removeButton)
				.AddGlue()
				.Add(defaultsButton)
				.Add(revertButton)
				.End()
			.End()
		;

	fFolderPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this), NULL,
		B_DIRECTORY_NODE, true);

	_ReloadPathList();

	_UpdateStatus();
	fStatusRunner = new BMessageRunner(BMessenger(this),
		new BMessage(kMsgUpdateStatus), kStatusPollInterval);

	BMessage registerObserver(kMsgRegisterProgressObserver);
	registerObserver.AddMessenger("observer", BMessenger(this));
	BMessenger(kIndexServerSignature).SendMessage(&registerObserver);
}


SettingsWindow::~SettingsWindow()
{
	BMessage unregisterObserver(kMsgUnregisterProgressObserver);
	unregisterObserver.AddMessenger("observer", BMessenger(this));
	BMessenger(kIndexServerSignature).SendMessage(&unregisterObserver);

	delete fStatusRunner;
	delete fFolderPanel;
}


void
SettingsWindow::_BuildAnalysersMenu(BMenuBar* menuBar)
{
	fAnalysersMenu = new BMenu(B_TRANSLATE("Analysers"));
	menuBar->AddItem(fAnalysersMenu);

	directory_which addOnDirectories[] = {
		B_USER_NONPACKAGED_ADDONS_DIRECTORY,
		B_USER_ADDONS_DIRECTORY,
		B_SYSTEM_NONPACKAGED_ADDONS_DIRECTORY,
		B_SYSTEM_ADDONS_DIRECTORY
	};

	std::vector<BString> seenNames;
	for (size_t i = 0; i < sizeof(addOnDirectories) / sizeof(directory_which);
			i++) {
		BPath path;
		if (find_directory(addOnDirectories[i], &path) != B_OK)
			continue;
		path.Append(kIndexServerDirectory.String());

		BDirectory directory(path.Path());
		entry_ref ref;
		while (directory.GetNextRef(&ref) == B_OK) {
			BString name(ref.name);
			bool seen = false;
			for (size_t j = 0; j < seenNames.size(); j++) {
				if (seenNames[j] == name) {
					seen = true;
					break;
				}
			}
			if (seen)
				continue;
			seenNames.push_back(name);

			BMessage* toggleMessage = new BMessage(kMsgToggleAnalyser);
			toggleMessage->AddString("name", name);
			BMenuItem* item = new BMenuItem(name.String(), toggleMessage);
			item->SetMarked(fSettings.IsAnalyserEnabled(name));
			fAnalysersMenu->AddItem(item);
		}
	}

	if (fAnalysersMenu->CountItems() == 0) {
		BMenuItem* placeholder = new BMenuItem(
			B_TRANSLATE("No analysers installed"), NULL);
		placeholder->SetEnabled(false);
		fAnalysersMenu->AddItem(placeholder);
	}
}


void
SettingsWindow::_ReloadPathList()
{
	for (int32 i = fPathListView->CountRows() - 1; i >= 0; i--) {
		BRow* row = fPathListView->RowAt(i);
		fPathListView->RemoveRow(row);
		delete row;
	}

	PathList paths = fSettings.Paths();
	for (size_t i = 0; i < paths.size(); i++) {
		BRow* row = new BRow();
		row->SetField(new BStringField(paths[i].String()), kPathColumn);
		fPathListView->AddRow(row);
	}

	BMenuItem* markedItem = fModeField->Menu()->FindItem(
		fSettings.Mode() == kBlacklistMode
			? B_TRANSLATE("All folders except the listed ones")
			: B_TRANSLATE("Only the listed folders"));
	if (markedItem != NULL)
		markedItem->SetMarked(true);
}


void
SettingsWindow::_AddRefs(BMessage* message)
{
	entry_ref ref;
	for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++) {
		BEntry entry(&ref, true);
		if (!entry.IsDirectory())
			continue;
		BPath path;
		if (entry.GetPath(&path) == B_OK)
			fSettings.AddPath(BString(path.Path()));
	}
	fSettings.Save();
	_ReloadPathList();
}


void
SettingsWindow::_RemoveSelectedPaths()
{
	BRow* row;
	while ((row = fPathListView->CurrentSelection()) != NULL) {
		BStringField* field = static_cast<BStringField*>(
			row->GetField(kPathColumn));
		fSettings.RemovePath(BString(field->String()));
		fPathListView->RemoveRow(row);
		delete row;
	}
	fSettings.Save();
}


void
SettingsWindow::_UpdateStatus()
{
	BMessenger indexServer(kIndexServerSignature);
	if (!indexServer.IsValid()) {
		fStatusView->SetText(B_TRANSLATE("Server not running"));
		return;
	}

	// Sent asynchronously (replyTo = this window) - this runs once from
	// the constructor, before Show(), and every kStatusPollInterval after
	// that. The old two-way SendMessage(message, &reply) blocked whatever
	// called this for as long as index_server took to answer - including,
	// the very first time, the constructor itself, which meant the window
	// never got shown at all if index_server was slow to respond right
	// then (e.g. waiting on a CLucene lock a Commit() was holding).
	BMessage query(kMsgGetStatus);
	if (indexServer.SendMessage(&query, this) != B_OK)
		fStatusView->SetText(B_TRANSLATE("Server not reachable"));
}


void
SettingsWindow::_HandleStatusReply(BMessage* reply)
{
	bool indexing = false;
	reply->FindBool("indexing", &indexing);
	fStatusView->SetText(indexing
		? B_TRANSLATE("Indexing" B_UTF8_ELLIPSIS)
		: B_TRANSLATE("Running"));
}


void
SettingsWindow::_HandleProgress(BMessage* message)
{
	int32 current = 0;
	int32 total = 0;
	message->FindInt32("current", &current);
	message->FindInt32("total", &total);
	if (total <= 0)
		return;

	if (current >= total) {
		fProgressBar->Hide();
		return;
	}

	BString path;
	message->FindString("path", &path);

	if (fProgressBar->IsHidden())
		fProgressBar->Show();

	BString trailing;
	trailing.SetToFormat("%ld / %ld", (long)current, (long)total);
	fProgressBar->SetTo(100.0f * current / total, path.String(),
		trailing.String());
}


void
SettingsWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgUpdateStatus:
			_UpdateStatus();
			break;

		case kMsgIndexProgress:
			_HandleProgress(message);
			break;

		case kMsgGetStatusReply:
			_HandleStatusReply(message);
			break;

		case kMsgModeChanged:
		{
			int32 mode;
			if (message->FindInt32("mode", &mode) == B_OK) {
				fSettings.SetMode((settings_mode)mode);
				fSettings.Save();
			}
			break;
		}

		case kMsgAddPath:
			if (message->HasRef("refs"))
				_AddRefs(message);
			else
				fFolderPanel->Show();
			break;

		case B_REFS_RECEIVED:
			_AddRefs(message);
			break;

		case kMsgRemovePath:
			_RemoveSelectedPaths();
			break;

		case kMsgDefaults:
			fSettings.ResetToDefaults();
			_ReloadPathList();
			break;

		case kMsgRevert:
			// Settings are saved as they're made; "revert" means picking up
			// whatever is currently on disk, in case it changed outside
			// this window.
			fSettings.Reload();
			_ReloadPathList();
			break;

		case kMsgToggleAnalyser:
		{
			BString name;
			if (message->FindString("name", &name) != B_OK)
				break;
			bool enabled = !fSettings.IsAnalyserEnabled(name);
			fSettings.SetAnalyserEnabled(name, enabled);
			fSettings.Save();
			BMenuItem* item = fAnalysersMenu->FindItem(name.String());
			if (item != NULL)
				item->SetMarked(enabled);
			break;
		}

		default:
			BWindow::MessageReceived(message);
	}
}


bool
SettingsWindow::QuitRequested()
{
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}
