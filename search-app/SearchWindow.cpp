/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "SearchWindow.h"

#include <Application.h>
#include <Button.h>
#include <Catalog.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <Entry.h>
#include <LayoutBuilder.h>
#include <Messenger.h>
#include <Path.h>
#include <Roster.h>
#include <StringView.h>
#include <TextControl.h>

#include "IndexServerPrivate.h"


#define DEBUG_SEARCH_WINDOW
#ifdef DEBUG_SEARCH_WINDOW
#include <stdio.h>
#include <string.h>
#	define STRACE(x...) printf("SearchWindow: " x)
#else
#	define STRACE(x...) ;
#endif


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "SearchWindow"


static const uint32 kMsgSearch = 'Srch';
static const uint32 kMsgOpenResult = 'Open';

static const int32 kPathColumn = 0;
static const int32 kScoreColumn = 1;


SearchWindow::SearchWindow()
	:
	BWindow(BRect(80, 80, 620, 480), B_TRANSLATE_SYSTEM_NAME("Index Search"),
		B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS)
{
	fQueryControl = new BTextControl("query", NULL, "",
		new BMessage(kMsgSearch));
	fQueryControl->SetModificationMessage(NULL);

	BButton* searchButton = new BButton("search", B_TRANSLATE("Search"),
		new BMessage(kMsgSearch));

	fResultsView = new BColumnListView("results", B_NAVIGABLE, B_PLAIN_BORDER);
	fResultsView->AddColumn(new BStringColumn(B_TRANSLATE("Path"), 400, 150,
		2000, B_TRUNCATE_MIDDLE), kPathColumn);
	fResultsView->AddColumn(new BStringColumn(B_TRANSLATE("Score"), 60, 40,
		120, B_TRUNCATE_END), kScoreColumn);
	fResultsView->SetInvocationMessage(new BMessage(kMsgOpenResult));

	fStatusView = new BStringView("status", "");
	fStatusView->SetAlignment(B_ALIGN_LEFT);

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_WINDOW_SPACING)
		.AddGroup(B_HORIZONTAL)
			.Add(fQueryControl)
			.Add(searchButton)
			.End()
		.Add(fResultsView)
		.Add(fStatusView)
		;

	fQueryControl->MakeFocus(true);
}


SearchWindow::~SearchWindow()
{
}


void
SearchWindow::_RunSearch()
{
	for (int32 i = fResultsView->CountRows() - 1; i >= 0; i--) {
		BRow* row = fResultsView->RowAt(i);
		fResultsView->RemoveRow(row);
		delete row;
	}

	BString queryString(fQueryControl->Text());
	STRACE("query text = \"%s\" (length %ld)\n", queryString.String(),
		(long)queryString.Length());
	if (queryString.Length() == 0) {
		fStatusView->SetText(B_TRANSLATE("Type something to search for."));
		return;
	}

	bigtime_t t0 = system_time();
	BMessenger indexServer(kIndexServerSignature);
	bigtime_t t1 = system_time();
	STRACE("BMessenger(%s) IsValid=%s (ctor took %" B_PRId64 " us)\n",
		kIndexServerSignature.String(), indexServer.IsValid() ? "true" : "false",
		t1 - t0);
	if (!indexServer.IsValid()) {
		fStatusView->SetText(B_TRANSLATE("index_server is not running."));
		return;
	}

	BMessage query(kMsgQuery);
	query.AddString("query", queryString);

	// Sent asynchronously (replyTo = this window, not the two-way
	// SendMessage(message, &reply) that waits right here) - a search can
	// take a while if it has to wait for a CLucene lock an in-progress
	// Commit() is holding, and blocking this thread for that blocks the
	// whole window's message loop (repaint, Cancel, everything) along
	// with it. The reply arrives later as a normal kMsgQueryReply message.
	fStatusView->SetText(B_TRANSLATE("Searching…"));
	fSearchSentTime = system_time();
	status_t sendStatus = indexServer.SendMessage(&query, this);
	bigtime_t t2 = system_time();
	STRACE("SendMessage status = %s (send call took %" B_PRId64 " us)\n",
		strerror(sendStatus), t2 - fSearchSentTime);
	if (sendStatus != B_OK)
		fStatusView->SetText(B_TRANSLATE("Could not reach index_server."));
}


void
SearchWindow::_HandleQueryReply(BMessage* reply)
{
	STRACE("reply received, round trip took %" B_PRId64 " us\n",
		system_time() - fSearchSentTime);

	entry_ref ref;
	float score;
	int32 count = 0;
	for (int32 i = 0; reply->FindRef("refs", i, &ref) == B_OK; i++) {
		reply->FindFloat("scores", i, &score);

		BPath path(&ref);
		STRACE("result %ld: %s (score %.3f)\n", (long)i, path.Path(), score);
		BRow* row = new BRow();
		row->SetField(new BStringField(path.Path()), kPathColumn);
		BString scoreText;
		scoreText.SetToFormat("%.2f", score);
		row->SetField(new BStringField(scoreText.String()), kScoreColumn);
		fResultsView->AddRow(row);
		count++;
	}

	int32 searchedVolumes = 0;
	reply->FindInt32("searchedVolumes", &searchedVolumes);
	STRACE("count=%ld searchedVolumes=%ld\n", (long)count,
		(long)searchedVolumes);

	BString status;
	if (searchedVolumes == 0) {
		status = B_TRANSLATE("No volume has a full text index yet.");
	} else {
		status.SetToFormat(
			B_TRANSLATE("%ld result(s) across %ld indexed volume(s)."),
			(long)count, (long)searchedVolumes);
	}
	fStatusView->SetText(status.String());
}


void
SearchWindow::_OpenSelected()
{
	BRow* row = fResultsView->CurrentSelection();
	if (row == NULL)
		return;

	BStringField* field = static_cast<BStringField*>(row->GetField(
		kPathColumn));
	BEntry entry(field->String());
	entry_ref ref;
	if (entry.GetRef(&ref) != B_OK)
		return;

	be_roster->Launch(&ref);
}


void
SearchWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgSearch:
			_RunSearch();
			break;

		case kMsgOpenResult:
			_OpenSelected();
			break;

		case kMsgQueryReply:
			_HandleQueryReply(message);
			break;

		default:
			BWindow::MessageReceived(message);
	}
}


bool
SearchWindow::QuitRequested()
{
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}
