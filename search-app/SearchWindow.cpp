/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "SearchWindow.h"

#include <stdlib.h>

#include <vector>

#include <Application.h>
#include <Button.h>
#include <Catalog.h>
#include <Entry.h>
#include <File.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <Mime.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Path.h>
#include <Roster.h>
#include <StringView.h>
#include <TextControl.h>

#include "IndexServerPrivate.h"
#include "SearchResultsView.h"


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
static const uint32 kMsgAbout = 'AbtR';

// Tracker's well-known signature (tracker_private.h's kTrackerSignature,
// not pulled in here to avoid depending on a private Tracker header for
// one string) - see _OpenSelected()'s comment for why a result is opened
// by messaging Tracker rather than launching an app directly.
static const char* const kTrackerSignature = "application/x-vnd.Be-TRAK";


// Long enough that a fast typist's keystrokes collapse into one search
// instead of one round trip per character; short enough to still feel
// live rather than like pressing a button.
static const bigtime_t kLiveFilterDelay = 350000;

// How many results one request (initial or "Load more") fetches.
static const int32 kResultsPerPage = 100;

// A cap on scanning a file client-side for the query words' first matching
// line (see _FindMatchingLine()) - not the same limit as the index side's
// kMaxIndexableFileSize, just meant to keep double-clicking a result from
// ever visibly stalling the UI on a pathologically large text file.
static const off_t kMaxLineSearchFileSize = 8 * 1024 * 1024;


namespace {


// Lucene query syntax (AND/OR/NOT, +required/-excluded, "phrases", a
// trailing */~ modifier) doesn't matter here - just want the plain words a
// person actually typed, to look for in the file's own text.
void
ExtractQueryWords(const BString& query, std::vector<BString>& words)
{
	BString cleaned(query);
	cleaned.ReplaceAll("\"", " ");

	int32 start = 0;
	while (start < cleaned.Length()) {
		int32 end = cleaned.FindFirst(' ', start);
		if (end < 0)
			end = cleaned.Length();

		BString word;
		cleaned.CopyInto(word, start, end - start);
		word.Trim();

		while (word.Length() > 0 && (word[0] == '+' || word[0] == '-'))
			word.Remove(0, 1);
		while (word.Length() > 0
			&& (word[word.Length() - 1] == '*'
				|| word[word.Length() - 1] == '~')) {
			word.Truncate(word.Length() - 1);
		}

		if (word.Length() > 0 && word != "AND" && word != "OR"
			&& word != "NOT") {
			words.push_back(word);
		}

		start = end + 1;
	}
}


// Our CLucene index has no per-match line/offset info to draw on (#21) -
// scan the file itself for the first line containing any of the query's
// words, the same underlying information Haiku's own text_search uses for
// its "open in editor" feature (it gets its line numbers from grep -n
// instead of an index).
int32
FindMatchingLine(const BPath& path, const std::vector<BString>& words)
{
	BFile file(path.Path(), B_READ_ONLY);
	off_t size;
	if (file.InitCheck() != B_OK || file.GetSize(&size) != B_OK
		|| size <= 0 || size > kMaxLineSearchFileSize) {
		return -1;
	}

	BString content;
	char* buffer = content.LockBuffer(size);
	ssize_t bytesRead = file.Read(buffer, size);
	content.UnlockBuffer(bytesRead > 0 ? bytesRead : 0);
	if (bytesRead <= 0)
		return -1;

	int32 lineNumber = 1;
	int32 lineStart = 0;
	while (lineStart <= content.Length()) {
		int32 lineEnd = content.FindFirst('\n', lineStart);
		if (lineEnd < 0)
			lineEnd = content.Length();

		BString line;
		content.CopyInto(line, lineStart, lineEnd - lineStart);
		for (size_t i = 0; i < words.size(); i++) {
			if (line.IFindFirst(words[i]) >= 0)
				return lineNumber;
		}

		lineNumber++;
		lineStart = lineEnd + 1;
	}

	return -1;
}


}	// namespace


SearchWindow::SearchWindow()
	:
	BWindow(BRect(80, 80, 660, 500), B_TRANSLATE_SYSTEM_NAME("Index Search"),
		B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS),
	fFilterRunner(NULL)
{
	BMenuBar* menuBar = new BMenuBar("menu");
	BMenu* fileMenu = new BMenu(B_TRANSLATE("File"));
	fileMenu->AddItem(new BMenuItem(B_TRANSLATE("About Index Search"),
		new BMessage(kMsgAbout)));
	fileMenu->AddSeparatorItem();
	fileMenu->AddItem(new BMenuItem(B_TRANSLATE("Quit"),
		new BMessage(B_QUIT_REQUESTED), 'Q'));
	menuBar->AddItem(fileMenu);

	fQueryControl = new BTextControl("query", NULL, "",
		new BMessage(kMsgSearch));
	fQueryControl->SetModificationMessage(new BMessage(kMsgLiveFilter));

	BButton* searchButton = new BButton("search", B_TRANSLATE("Search"),
		new BMessage(kMsgSearch));

	fResultsView = new SearchResultsView("results");
	fResultsView->SetInvocationMessage(new BMessage(kMsgOpenResult));

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
	BLayoutBuilder::Group<>(this, B_VERTICAL, 0.0f)
		.Add(menuBar)
		.AddGroup(B_VERTICAL, B_USE_WINDOW_SPACING)
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
			.End()
		;

	fQueryControl->MakeFocus(true);

	// Keeps the query row, status row and results list usable when the
	// window is shrunk - the results list only ever gives up space down to
	// its own minimum, so without a window minimum the lower rows could be
	// pushed out of view.
	float minWidth, minHeight, maxWidth, maxHeight;
	GetSizeLimits(&minWidth, &maxWidth, &minHeight, &maxHeight);
	SetSizeLimits(minWidth, maxWidth, 400, maxHeight);
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
	fResultsView->SetQuery(queryString);

	if (queryString.Length() == 0) {
		// Nothing in flight is worth waiting for a reply to clear - do it
		// now. Also bumps the token, so a reply for whatever was still
		// outstanding gets dropped as stale instead of repopulating a
		// list the user just emptied.
		++fPendingQueryToken;
		fResultsView->ClearResults();
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
		fResultsView->ClearResults();
	}

	entry_ref ref;
	float score;
	int32 count = 0;
	for (int32 i = 0; reply->FindRef("refs", i, &ref) == B_OK; i++) {
		reply->FindFloat("scores", i, &score);
		fResultsView->AddResult(ref, score);
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
	entry_ref ref;
	if (!fResultsView->GetSelectedRef(&ref))
		return;

	BNode node(&ref);
	BNodeInfo nodeInfo(&node);
	char mimeType[B_MIME_TYPE_LENGTH];
	bool isText = node.InitCheck() == B_OK
		&& nodeInfo.GetType(mimeType) == B_OK
		&& strncasecmp(mimeType, "text/", 5) == 0;

	// "Open at the matching line" (#21) only makes sense for a file an
	// editor can meaningfully jump to a line in - a translated PDF or
	// image has no line numbers of its own to speak of, so this is
	// deliberately narrower than what actually gets indexed.
	int32 lineNumber = -1;
	if (isText) {
		std::vector<BString> words;
		ExtractQueryWords(BString(fQueryControl->Text()), words);
		if (!words.empty())
			lineNumber = FindMatchingLine(BPath(&ref), words);
	}

	BMessage message(B_REFS_RECEIVED);
	message.AddRef("refs", &ref);
	if (lineNumber > 0)
		message.AddInt32("be:line", lineNumber);

	// Routed through Tracker rather than resolving/launching the target
	// app ourselves. This is the same path /bin/open uses for its own
	// "file:line" argument - Tracker's own ref-open handling already
	// knows how to pick the right app for a ref, and specifically copies
	// over any "be:*" fields (see Tracker.cpp's own comment: "e.g.
	// /bin/open may include be:line and be:column") to whatever it
	// launches. Resolving a preferred app ourselves (BMimeType::
	// GetPreferredApp(), the approach Haiku's own text_search uses) is
	// both more code and less reliable - it returned "no such file" for
	// plain text/plain on this system, despite `open somefile.txt:3`
	// correctly landing on the right line in StyledEdit moments earlier.
	BMessenger tracker(kTrackerSignature);
	if (tracker.IsValid())
		tracker.SendMessage(&message);
	else
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

		case kMsgAbout:
			be_app->PostMessage(B_ABOUT_REQUESTED);
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
