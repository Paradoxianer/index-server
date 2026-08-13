/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "SearchWindow.h"

#include <stdlib.h>

#include <Application.h>
#include <Bitmap.h>
#include <Button.h>
#include <Catalog.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <Entry.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <NodeInfo.h>
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
static const uint32 kMsgLiveFilter = 'LFlt';
static const uint32 kMsgLoadMore = 'LdMr';
static const uint32 kMsgOpenResult = 'Open';

static const int32 kNameColumn = 0;
static const int32 kLocationColumn = 1;
static const int32 kScoreColumn = 2;

// Long enough that a fast typist's keystrokes collapse into one search
// instead of one round trip per character; short enough to still feel
// live rather than like pressing a button.
static const bigtime_t kLiveFilterDelay = 350000;

// How many results one request (initial or "Load more") fetches.
static const int32 kResultsPerPage = 100;


namespace {


// Combines an icon with the filename in one field, the same approach
// DriveSetup's PartitionList.cpp uses - the stock ColumnTypes.h only
// offers BBitmapField and BStringField separately, not a field type
// that draws both together.
class IconStringField : public BStringField {
public:
	IconStringField(BBitmap* bitmap, const char* string)
		:
		BStringField(string),
		fBitmap(bitmap)
	{
	}

	virtual ~IconStringField()
	{
		delete fBitmap;
	}

	const BBitmap* Bitmap() const
	{
		return fBitmap;
	}

private:
	BBitmap* fBitmap;
};


// Draws an IconStringField's bitmap and text side by side. Subclassing
// BStringColumn (rather than BTitledColumn directly, as DriveSetup does)
// means CompareFields() and AcceptsField() - both string-based - come for
// free; only the drawing needs to account for the icon.
class IconNameColumn : public BStringColumn {
public:
	IconNameColumn(const char* title, float width, float minWidth,
		float maxWidth, uint32 truncateMode)
		:
		BStringColumn(title, width, minWidth, maxWidth, truncateMode)
	{
	}

	virtual void DrawField(BField* field, BRect rect, BView* parent)
	{
		IconStringField* iconField = dynamic_cast<IconStringField*>(field);
		if (iconField == NULL) {
			BStringColumn::DrawField(field, rect, parent);
			return;
		}

		const BBitmap* bitmap = iconField->Bitmap();
		const float kIconTextGap = 4.0f;
		float iconWidth = bitmap != NULL ? bitmap->Bounds().Width() + 1 : 0;

		if (bitmap != NULL) {
			float y = rect.top
				+ (rect.Height() - bitmap->Bounds().Height()) / 2;
			parent->SetDrawingMode(B_OP_ALPHA);
			parent->DrawBitmap(bitmap, BPoint(rect.left, y));
			parent->SetDrawingMode(B_OP_OVER);
		}

		BRect textRect(rect);
		textRect.left += iconWidth + kIconTextGap;

		float width = textRect.Width();
		if (width != iconField->Width()) {
			BString truncated(iconField->String());
			parent->TruncateString(&truncated, B_TRUNCATE_MIDDLE, width + 2);
			iconField->SetClippedString(truncated.String());
			iconField->SetWidth(width);
		}
		DrawString(iconField->ClippedString(), parent, textRect);
	}

	virtual float GetPreferredWidth(BField* field, BView* parent) const
	{
		IconStringField* iconField = dynamic_cast<IconStringField*>(field);
		if (iconField == NULL)
			return BStringColumn::GetPreferredWidth(field, parent);

		float width = BStringColumn::GetPreferredWidth(field, parent);
		const BBitmap* bitmap = iconField->Bitmap();
		if (bitmap != NULL)
			width += bitmap->Bounds().Width() + 5;
		return width;
	}
};


// Scores are formatted text ("0.85"), but sorting them as text would put
// "10.00" before "2.00" - compare the actual numeric value instead.
class ScoreColumn : public BStringColumn {
public:
	ScoreColumn(const char* title, float width, float minWidth,
		float maxWidth, uint32 truncateMode)
		:
		BStringColumn(title, width, minWidth, maxWidth, truncateMode)
	{
	}

	virtual int CompareFields(BField* field1, BField* field2)
	{
		double value1 = atof(((BStringField*)field1)->String());
		double value2 = atof(((BStringField*)field2)->String());
		if (value1 < value2)
			return -1;
		if (value1 > value2)
			return 1;
		return 0;
	}
};


// Keeps the entry_ref a row was built from, so opening it doesn't need to
// reconstruct a path from truncated/split display text.
class ResultRow : public BRow {
public:
	ResultRow(const entry_ref& ref)
		:
		BRow(),
		fRef(ref)
	{
	}

	const entry_ref& Ref() const
	{
		return fRef;
	}

private:
	entry_ref fRef;
};


}	// namespace


SearchWindow::SearchWindow()
	:
	BWindow(BRect(80, 80, 660, 500), B_TRANSLATE_SYSTEM_NAME("Index Search"),
		B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS),
	fFilterRunner(NULL)
{
	fQueryControl = new BTextControl("query", NULL, "",
		new BMessage(kMsgSearch));
	fQueryControl->SetModificationMessage(new BMessage(kMsgLiveFilter));

	BButton* searchButton = new BButton("search", B_TRANSLATE("Search"),
		new BMessage(kMsgSearch));

	fResultsView = new BColumnListView("results", B_NAVIGABLE, B_PLAIN_BORDER);
	fResultsView->AddColumn(new IconNameColumn(B_TRANSLATE("Name"), 220, 100,
		600, B_TRUNCATE_MIDDLE), kNameColumn);
	fResultsView->AddColumn(new BStringColumn(B_TRANSLATE("Location"), 260,
		100, 2000, B_TRUNCATE_MIDDLE), kLocationColumn);
	fResultsView->AddColumn(new ScoreColumn(B_TRANSLATE("Score"), 60, 40,
		120, B_TRUNCATE_END), kScoreColumn);
	fResultsView->SetInvocationMessage(new BMessage(kMsgOpenResult));
	fResultsView->SetSortingEnabled(true);

	fLoadMoreButton = new BButton("loadMore", B_TRANSLATE("Load more results"),
		new BMessage(kMsgLoadMore));
	fLoadMoreButton->Hide();

	fPendingQueryToken = 0;
	fCurrentOffset = 0;
	fTotalHits = 0;

	fStatusView = new BStringView("status", "");
	fStatusView->SetAlignment(B_ALIGN_LEFT);

	// Every row defaults to layout weight 1.0, splitting extra vertical
	// space equally - the single-line query/load-more/status rows were
	// getting stretched exactly like the results list, which is why the
	// window opened at roughly half height (nothing to stretch into yet)
	// and the visible list shrank the moment the load-more row appeared
	// and claimed its own equal share. Weight 0 pins those rows to their
	// natural height instead, leaving the results view the only one that
	// grows or shrinks with the window.
	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_WINDOW_SPACING)
		.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING, 0.0f)
			.Add(fQueryControl)
			.Add(searchButton)
			.End()
		.Add(fResultsView)
		.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING, 0.0f)
			.Add(fLoadMoreButton)
			.AddGlue()
			.End()
		.Add(fStatusView, 0.0f)
		;

	fQueryControl->MakeFocus(true);
}


SearchWindow::~SearchWindow()
{
	delete fFilterRunner;
}


// Debounces live-filter keystrokes: each modification cancels any pending
// search and schedules a new one kLiveFilterDelay out, so a burst of
// typing collapses into a single query instead of one per character.
void
SearchWindow::_ScheduleLiveFilter()
{
	delete fFilterRunner;
	fFilterRunner = NULL;

	if (BString(fQueryControl->Text()).Length() == 0) {
		// Nothing to debounce - clear immediately, same as an empty
		// explicit search.
		_RunSearch();
		return;
	}

	BMessage message(kMsgSearch);
	fFilterRunner = new BMessageRunner(BMessenger(this), &message,
		kLiveFilterDelay, 1);
}


void
SearchWindow::_RunSearch()
{
	BString queryString(fQueryControl->Text());
	STRACE("query text = \"%s\" (length %ld)\n", queryString.String(),
		(long)queryString.Length());
	if (queryString.Length() == 0) {
		// Nothing in flight is worth waiting for a reply to clear - do it
		// now. Also bumps the token, so a reply for whatever was still
		// outstanding gets dropped as stale instead of repopulating a
		// list the user just emptied.
		++fPendingQueryToken;
		for (int32 i = fResultsView->CountRows() - 1; i >= 0; i--) {
			BRow* row = fResultsView->RowAt(i);
			fResultsView->RemoveRow(row);
			delete row;
		}
		if (!fLoadMoreButton->IsHidden())
			fLoadMoreButton->Hide();
		fCurrentOffset = 0;
		fTotalHits = 0;
		fStatusView->SetText(B_TRANSLATE("Type something to search for."));
		return;
	}

	fCurrentOffset = 0;
	_SendQuery(0);
}


void
SearchWindow::_LoadMore()
{
	_SendQuery(fCurrentOffset);
}


// Shared by a fresh search (offset 0) and "Load more" (offset = however
// many results are already showing). The existing rows are deliberately
// left in place until the reply actually has replacements ready - clearing
// up front and only then waiting on the round trip (which also fetches an
// icon per result, not free for a full page) left the list visibly empty
// for that whole stretch on every fresh search.
void
SearchWindow::_SendQuery(int32 offset)
{
	BString queryString(fQueryControl->Text());

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
	query.AddInt32("offset", offset);
	query.AddInt32("maxResults", kResultsPerPage);
	query.AddInt32("queryToken", ++fPendingQueryToken);

	// Sent asynchronously (replyTo = this window, not the two-way
	// SendMessage(message, &reply) that waits right here) - a search can
	// take a while if it has to wait for a CLucene lock an in-progress
	// Commit() is holding, and blocking this thread for that blocks the
	// whole window's message loop (repaint, Cancel, everything) along
	// with it. The reply arrives later as a normal kMsgQueryReply message.
	fStatusView->SetText(offset == 0 ? B_TRANSLATE("Searching…")
		: B_TRANSLATE("Loading more…"));
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
	int32 token;
	if (reply->FindInt32("queryToken", &token) == B_OK
		&& token != fPendingQueryToken) {
		// A newer query has since been sent (e.g. the user kept typing
		// during live filtering, or clicked "Load more" twice) - this
		// reply is for a superseded query and may have arrived after
		// that newer one's reply already did; applying it now would show
		// stale results, possibly in the wrong place (a fresh search
		// appended onto instead of replacing what's showing).
		STRACE("dropping stale reply (token %" B_PRId32 ", pending %"
			B_PRId32 ")\n", token, fPendingQueryToken);
		return;
	}

	STRACE("reply received, round trip took %" B_PRId64 " us\n",
		system_time() - fSearchSentTime);

	// This is the first (and, until "Load more" is used again, only)
	// reply for the current query text - now that replacement rows are
	// actually ready, clear whatever the previous query left behind.
	if (fCurrentOffset == 0) {
		for (int32 i = fResultsView->CountRows() - 1; i >= 0; i--) {
			BRow* row = fResultsView->RowAt(i);
			fResultsView->RemoveRow(row);
			delete row;
		}
	}

	entry_ref ref;
	float score;
	int32 count = 0;
	for (int32 i = 0; reply->FindRef("refs", i, &ref) == B_OK; i++) {
		reply->FindFloat("scores", i, &score);

		BPath path(&ref);
		BPath parent;
		path.GetParent(&parent);
		STRACE("result %ld: %s (score %.3f)\n", (long)i, path.Path(), score);

		BBitmap* icon = new BBitmap(BRect(0, 0, 15, 15), B_RGBA32);
		if (BNodeInfo::GetTrackerIcon(&ref, icon, B_MINI_ICON) != B_OK) {
			delete icon;
			icon = NULL;
		}

		ResultRow* row = new ResultRow(ref);
		row->SetField(new IconStringField(icon, ref.name), kNameColumn);
		row->SetField(new BStringField(parent.Path()), kLocationColumn);
		BString scoreText;
		scoreText.SetToFormat("%.2f", score);
		row->SetField(new BStringField(scoreText.String()), kScoreColumn);
		fResultsView->AddRow(row);
		count++;
	}
	fCurrentOffset += count;

	reply->FindInt32("totalHits", &fTotalHits);
	// Hide()/Show() nest (each Hide() needs its own matching Show()), so
	// only call whichever one actually changes the current state -
	// calling Hide() twice in a row would need two Show()s to undo.
	bool shouldShowLoadMore = fCurrentOffset > 0 && fCurrentOffset < fTotalHits;
	if (shouldShowLoadMore && fLoadMoreButton->IsHidden())
		fLoadMoreButton->Show();
	else if (!shouldShowLoadMore && !fLoadMoreButton->IsHidden())
		fLoadMoreButton->Hide();

	int32 searchedVolumes = 0;
	reply->FindInt32("searchedVolumes", &searchedVolumes);
	STRACE("count=%ld searchedVolumes=%ld totalHits=%ld\n", (long)count,
		(long)searchedVolumes, (long)fTotalHits);

	BString status;
	if (searchedVolumes == 0) {
		status = B_TRANSLATE("No volume has a full text index yet.");
	} else {
		status.SetToFormat(
			B_TRANSLATE("%ld of %ld result(s) shown, across %ld indexed "
				"volume(s)."),
			(long)fCurrentOffset, (long)fTotalHits, (long)searchedVolumes);
	}
	fStatusView->SetText(status.String());
}


void
SearchWindow::_OpenSelected()
{
	ResultRow* row = static_cast<ResultRow*>(fResultsView->CurrentSelection());
	if (row == NULL)
		return;

	entry_ref ref = row->Ref();
	be_roster->Launch(&ref);
}


void
SearchWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgSearch:
			_RunSearch();
			break;

		case kMsgLiveFilter:
			_ScheduleLiveFilter();
			break;

		case kMsgLoadMore:
			_LoadMore();
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
