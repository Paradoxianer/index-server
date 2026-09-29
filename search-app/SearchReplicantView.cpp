/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "SearchReplicantView.h"

#include <Catalog.h>
#include <ControlLook.h>
#include <Entry.h>
#include <Message.h>
#include <Messenger.h>
#include <Path.h>
#include <Roster.h>
#include <Window.h>

#include "IndexServerPrivate.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "SearchReplicantView"


// This view's own executable's app signature - see this class's own
// header comment for why that's what "add_on" needs to be, not
// index_server's.
static const char* const kSearchAppSignature
	= "application/x-vnd.Haiku-IndexServerSearch";

// Tracker's well-known signature - see SearchWindow.cpp's own copy of
// this same constant for why a result is opened by messaging Tracker
// rather than launching an app directly. Kept as its own local copy
// here too rather than shared, the same reasoning IndexServerPrivate.h's
// own comments give for not centralizing every such constant.
static const char* const kTrackerSignature = "application/x-vnd.Be-TRAK";

// How many matches to actually list by name - a Desktop replicant is a
// small corner-of-the-screen widget, not a results window; anything
// beyond this only shows as a "+N more" count.
static const int32 kMaxShownMatches = 4;


SearchReplicantView::SearchReplicantView(BRect frame, const char* query)
	:
	BView(frame, "SearchReplicantView", B_FOLLOW_NONE, B_WILL_DRAW),
	fQuery(query),
	fMonitorToken(-1)
{
	_Init();
}


SearchReplicantView::SearchReplicantView(BMessage* archive)
	:
	BView(archive),
	fMonitorToken(-1)
{
	archive->FindString("query", &fQuery);
	_Init();
}


void
SearchReplicantView::_Init()
{
	SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
	SetLowColor(ViewColor());
	SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
}


SearchReplicantView::~SearchReplicantView()
{
}


SearchReplicantView*
SearchReplicantView::Instantiate(BMessage* archive)
{
	if (!validate_instantiation(archive, "SearchReplicantView"))
		return NULL;
	return new SearchReplicantView(archive);
}


status_t
SearchReplicantView::Archive(BMessage* archive, bool deep) const
{
	status_t status = BView::Archive(archive, deep);
	if (status == B_OK)
		status = archive->AddString("query", fQuery);

	// Order matters here - see this file's own reference (CalcView.cpp
	// in a full Haiku source tree) for why "add_on" is added before, and
	// "class" only right at the end: instantiate_object() reads "class"
	// last, after everything else describing what to reconstruct is
	// already in the message.
	if (status == B_OK)
		status = archive->AddString("add_on", kSearchAppSignature);
	if (status == B_OK)
		status = archive->AddString("class", "SearchReplicantView");

	return status;
}


void
SearchReplicantView::AttachedToWindow()
{
	BView::AttachedToWindow();
	if (Parent() != NULL) {
		SetViewColor(Parent()->ViewColor());
		SetLowColor(ViewColor());
	}
	_StartMonitor();
}


void
SearchReplicantView::DetachedFromWindow()
{
	_StopMonitor();
	BView::DetachedFromWindow();
}


void
SearchReplicantView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgStartQueryMonitorReply:
		{
			int32 token;
			if (message->FindInt32("monitorToken", &token) == B_OK)
				fMonitorToken = token;
			break;
		}

		case kMsgQueryMonitorUpdate:
		{
			// A stale reply for a query this replicant has since moved
			// on from via SetQuery() (the embedded preview in
			// SearchWindow re-registers every time the user searches for
			// something else) - the old monitor's own kMsgStopQueryMonitor
			// may not have reached index_server yet when this was sent.
			int32 token;
			if (message->FindInt32("monitorToken", &token) != B_OK
					|| token != fMonitorToken) {
				break;
			}

			entry_ref ref;
			float score;
			for (int32 i = 0; message->FindRef("addedRefs", i, &ref)
					== B_OK; i++) {
				message->FindFloat("addedScores", i, &score);
				BPath path(&ref);
				fMatches[path.Path()] = ref;
			}
			for (int32 i = 0; message->FindRef("removedRefs", i, &ref)
					== B_OK; i++) {
				BPath path(&ref);
				fMatches.erase(path.Path());
			}
			Invalidate();
			break;
		}

		default:
			BView::MessageReceived(message);
	}
}


void
SearchReplicantView::Draw(BRect updateRect)
{
	BRect bounds(Bounds());
	SetHighColor(ui_color(B_PANEL_BACKGROUND_COLOR));
	FillRect(bounds);

	font_height fontHeight;
	GetFontHeight(&fontHeight);
	float lineHeight = fontHeight.ascent + fontHeight.descent
		+ fontHeight.leading;
	float x = 4;
	float y = fontHeight.ascent + 4;

	SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
	BString header;
	if (fQuery.Length() == 0)
		header = B_TRANSLATE("(no query)");
	else
		header.SetToFormat("\"%s\"", fQuery.String());
	BString truncatedHeader(header);
	TruncateString(&truncatedHeader, B_TRUNCATE_END, bounds.Width() - 2 * x);
	DrawString(truncatedHeader.String(), BPoint(x, y));
	y += lineHeight;

	BString countLine;
	countLine.SetToFormat(
		B_TRANSLATE_COMMENT("%ld match(es)",
			"count line under the query in the Desktop replicant widget"),
		(long)fMatches.size());
	DrawString(countLine.String(), BPoint(x, y));
	y += lineHeight;

	int32 shown = 0;
	for (std::map<BString, entry_ref>::const_iterator it = fMatches.begin();
			it != fMatches.end() && shown < kMaxShownMatches; ++it) {
		if (y + lineHeight > bounds.bottom)
			break;
		BString truncatedName(it->second.name);
		TruncateString(&truncatedName, B_TRUNCATE_END, bounds.Width() - 2 * x);
		DrawString(truncatedName.String(), BPoint(x, y));
		y += lineHeight;
		shown++;
	}

	int32 remaining = (int32)fMatches.size() - shown;
	if (remaining > 0 && y + lineHeight <= bounds.bottom) {
		BString more;
		more.SetToFormat(B_TRANSLATE("+%ld more"), (long)remaining);
		DrawString(more.String(), BPoint(x, y));
	}
}


void
SearchReplicantView::MouseDown(BPoint where)
{
	font_height fontHeight;
	GetFontHeight(&fontHeight);
	float lineHeight = fontHeight.ascent + fontHeight.descent
		+ fontHeight.leading;

	// The header and count line always take up the first two lines (see
	// Draw()) - only a click below those can be one of the listed
	// matches.
	int32 row = (int32)((where.y - 2 * lineHeight) / lineHeight);
	if (row < 0)
		return;

	int32 i = 0;
	for (std::map<BString, entry_ref>::const_iterator it = fMatches.begin();
			it != fMatches.end(); ++it, i++) {
		if (i == row) {
			_OpenMatch(it->second);
			return;
		}
	}
}


void
SearchReplicantView::SetQuery(const BString& query)
{
	if (query == fQuery)
		return;

	_StopMonitor();
	fQuery = query;
	fMatches.clear();
	if (Window() != NULL)
		_StartMonitor();
	Invalidate();
}


void
SearchReplicantView::_StartMonitor()
{
	if (fQuery.Length() == 0)
		return;

	BMessenger indexServer(kIndexServerSignature.String());
	if (!indexServer.IsValid())
		return;

	BMessage request(kMsgStartQueryMonitor);
	request.AddString("query", fQuery);
	request.AddInt32("maxResults", 20);
	request.AddMessenger("target", BMessenger(this));

	// Sent asynchronously with this view as the reply target (same
	// reasoning as SearchWindow's own _SendQuery()) - the reply arrives
	// later as kMsgStartQueryMonitorReply via MessageReceived().
	indexServer.SendMessage(&request, this);
}


void
SearchReplicantView::_StopMonitor()
{
	if (fMonitorToken < 0)
		return;

	BMessenger indexServer(kIndexServerSignature.String());
	if (indexServer.IsValid()) {
		BMessage stop(kMsgStopQueryMonitor);
		stop.AddInt32("monitorToken", fMonitorToken);
		indexServer.SendMessage(&stop);
	}
	fMonitorToken = -1;
	fMatches.clear();
}


void
SearchReplicantView::_OpenMatch(const entry_ref& ref)
{
	// Routed through Tracker rather than resolving/launching the target
	// app ourselves - see SearchWindow.cpp's _OpenSelected() for why.
	BMessage message(B_REFS_RECEIVED);
	message.AddRef("refs", &ref);

	BMessenger tracker(kTrackerSignature);
	if (tracker.IsValid())
		tracker.SendMessage(&message);
	else
		be_roster->Launch(&ref);
}
