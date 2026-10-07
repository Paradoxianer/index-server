/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "SearchResultsView.h"

#include <stdlib.h>
#include <string.h>

#include <Bitmap.h>
#include <Catalog.h>
#include <ColumnTypes.h>
#include <Dragger.h>
#include <Entry.h>
#include <Message.h>
#include <Messenger.h>
#include <Mime.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Path.h>
#include <Roster.h>

#include "IndexServerPrivate.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "SearchResultsView"


// This view's own executable's app signature - see SearchResultsView.h
// for why "add_on" is that and not index_server's.
static const char* const kSearchAppSignature
	= "application/x-vnd.Haiku-IndexServerSearch";

// Tracker's well-known signature - see SearchWindow.cpp's own copy of
// this constant for why a result is opened by messaging Tracker rather
// than launching an app directly.
static const char* const kTrackerSignature = "application/x-vnd.Be-TRAK";

static const uint32 kMsgOpenRow = 'OpRw';

static const int32 kNameColumn = 0;
static const int32 kLocationColumn = 1;
static const int32 kScoreColumn = 2;
static const int32 kSizeColumn = 3;
static const int32 kModifiedColumn = 4;
static const int32 kKindColumn = 5;

// Matches what a live replicant asks index_server for - a fixed-size
// widget on the Desktop can't usefully show more than this anyway.
static const int32 kReplicantMaxResults = 100;

// Size of the Desktop replicant when it's first instantiated, before the
// user resizes it.
static const float kReplicantWidth = 420.0f;
static const float kReplicantHeight = 220.0f;

// Size of the BDragger handle in the bottom-right corner.
static const float kDraggerSize = 7.0f;


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


// Same source Tracker's own "Kind" column uses: the MIME type's
// registered short description ("JPEG image"), falling back to the raw
// MIME string if the type isn't registered with one, and finally to
// nothing at all rather than a placeholder for a file with no MIME type.
BString
KindDescriptionFor(const entry_ref& ref)
{
	BNode node(&ref);
	BNodeInfo nodeInfo(&node);
	char mimeType[B_MIME_TYPE_LENGTH];
	if (node.InitCheck() != B_OK || nodeInfo.GetType(mimeType) != B_OK)
		return BString();

	BMimeType type(mimeType);
	char description[B_MIME_TYPE_LENGTH];
	if (type.GetShortDescription(description) == B_OK)
		return BString(description);

	return BString(mimeType);
}


}	// namespace


SearchResultsView::SearchResultsView(const char* name)
	:
	BColumnListView(name, B_NAVIGABLE, B_PLAIN_BORDER),
	fMonitorToken(-1),
	fDragger(NULL)
{
	_Init();
}


SearchResultsView::SearchResultsView(BMessage* archive)
	:
	BColumnListView("results", B_NAVIGABLE, B_PLAIN_BORDER),
	fMonitorToken(-1),
	fDragger(NULL)
{
	archive->FindString("query", &fQuery);

	BRect frame;
	if (archive->FindRect("frame", &frame) == B_OK)
		ResizeTo(frame.Width(), frame.Height());
	else
		ResizeTo(kReplicantWidth, kReplicantHeight);

	_Init();
}


SearchResultsView::~SearchResultsView()
{
}


void
SearchResultsView::_Init()
{
	AddColumn(new IconNameColumn(B_TRANSLATE("Name"), 220, 100, 600,
		B_TRUNCATE_MIDDLE), kNameColumn);
	AddColumn(new BStringColumn(B_TRANSLATE("Location"), 260, 100, 2000,
		B_TRUNCATE_MIDDLE), kLocationColumn);
	AddColumn(new ScoreColumn(B_TRANSLATE("Score"), 60, 40, 120,
		B_TRUNCATE_END), kScoreColumn);

	// Off by default (right-click the header to bring them back, a
	// BColumnListView feature that needs no extra code here) - Score and
	// Location cover the common case, and a column full of icon-sized
	// bitmaps doesn't get much wider by showing more of them at once.
	BSizeColumn* sizeColumn = new BSizeColumn(B_TRANSLATE("Size"), 70, 40,
		200);
	AddColumn(sizeColumn, kSizeColumn);
	sizeColumn->SetVisible(false);

	BDateColumn* modifiedColumn = new BDateColumn(B_TRANSLATE("Modified"),
		140, 100, 300);
	AddColumn(modifiedColumn, kModifiedColumn);
	modifiedColumn->SetVisible(false);

	BStringColumn* kindColumn = new BStringColumn(B_TRANSLATE("Kind"), 140,
		80, 300, B_TRUNCATE_END);
	AddColumn(kindColumn, kKindColumn);
	kindColumn->SetVisible(false);

	SetInvocationMessage(new BMessage(kMsgOpenRow));
	SetSortingEnabled(true);

	// Hangs off the list's own corner, so dragging the handle drags the
	// whole list - what gets archived, and later instantiated on the
	// Desktop, is this view (see Archive()). B_FOLLOW_NONE because its
	// position is kept correct by hand, in _PositionDragger() - the
	// window-embedded list is still 0x0 here (its real size only exists
	// once the window's layout pass runs, well after this constructor),
	// so there's no correct corner to compute yet at this point anyway.
	fDragger = new BDragger(BRect(0, 0, kDraggerSize - 1, kDraggerSize - 1),
		this, B_FOLLOW_NONE);
	AddChild(fDragger);
	_PositionDragger();
}


void
SearchResultsView::_PositionDragger()
{
	if (fDragger == NULL)
		return;

	BRect bounds(Bounds());
	fDragger->MoveTo(bounds.right - kDraggerSize + 1,
		bounds.bottom - kDraggerSize + 1);
}


void
SearchResultsView::FrameResized(float width, float height)
{
	BColumnListView::FrameResized(width, height);
	_PositionDragger();
}


SearchResultsView*
SearchResultsView::Instantiate(BMessage* archive)
{
	if (!validate_instantiation(archive, "SearchResultsView"))
		return NULL;
	return new SearchResultsView(archive);
}


status_t
SearchResultsView::Archive(BMessage* archive, bool deep) const
{
	// Shallow on purpose: the BDragger child is recreated by the
	// constructor on the other end, and the rows are rebuilt live from the
	// query monitor rather than archived - only the query and the frame
	// need to survive the trip.
	status_t status = BColumnListView::Archive(archive, false);
	if (status == B_OK)
		status = archive->AddString("query", fQuery);
	if (status == B_OK)
		status = archive->AddRect("frame", Frame());

	// Order matters - instantiate_object() reads "class" last, as the
	// most derived class in the archive (see CalcView.cpp's Archive()).
	if (status == B_OK)
		status = archive->AddString("add_on", kSearchAppSignature);
	if (status == B_OK)
		status = archive->AddString("class", "SearchResultsView");

	return status;
}


void
SearchResultsView::AttachedToWindow()
{
	BColumnListView::AttachedToWindow();
	// Sized before it was attached (the constructor's ResizeTo()), so the
	// list never got its FrameResized() and never laid out its own header
	// and scrollbars.
	FrameResized(Bounds().Width(), Bounds().Height());
	_StartMonitor();
}


void
SearchResultsView::DetachedFromWindow()
{
	_StopMonitor();
	BColumnListView::DetachedFromWindow();
}


void
SearchResultsView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgOpenRow:
		{
			entry_ref ref;
			if (GetSelectedRef(&ref))
				_OpenRow(ref);
			break;
		}

		case kMsgStartQueryMonitorReply:
		{
			int32 token;
			if (message->FindInt32("monitorToken", &token) == B_OK)
				fMonitorToken = token;
			break;
		}

		case kMsgQueryMonitorUpdate:
		{
			// A stale reply for a query this view has since moved on from
			// (SetQuery() re-registers) - its own kMsgStopQueryMonitor may
			// not have reached index_server yet when this was sent.
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
				AddResult(ref, score);
			}
			for (int32 i = 0; message->FindRef("removedRefs", i, &ref)
					== B_OK; i++) {
				RemoveResult(ref);
			}
			break;
		}

		default:
			BColumnListView::MessageReceived(message);
	}
}


void
SearchResultsView::SetQuery(const BString& query)
{
	if (query == fQuery)
		return;

	fQuery = query;
	_StopMonitor();
	ClearResults();
	if (Window() != NULL)
		_StartMonitor();
}


const BString&
SearchResultsView::Query() const
{
	return fQuery;
}


void
SearchResultsView::AddResult(const entry_ref& ref, float score)
{
	// Re-adding an existing ref (a score changed, per a monitor update) is
	// the same as replacing it - there's only ever one row per file.
	RemoveResult(ref);

	BPath path(&ref);
	BPath parent;
	path.GetParent(&parent);

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

	BEntry entry(&ref);
	off_t size = 0;
	entry.GetSize(&size);
	row->SetField(new BSizeField(size), kSizeColumn);

	time_t modified = 0;
	entry.GetModificationTime(&modified);
	row->SetField(new BDateField(&modified), kModifiedColumn);

	row->SetField(new BStringField(KindDescriptionFor(ref)), kKindColumn);

	AddRow(row);
}


void
SearchResultsView::RemoveResult(const entry_ref& ref)
{
	for (int32 i = CountRows() - 1; i >= 0; i--) {
		ResultRow* row = static_cast<ResultRow*>(RowAt(i));
		const entry_ref& rowRef = row->Ref();
		if (rowRef.device == ref.device && rowRef.directory == ref.directory
				&& strcmp(rowRef.name, ref.name) == 0) {
			RemoveRow(row);
			delete row;
		}
	}
}


void
SearchResultsView::ClearResults()
{
	for (int32 i = CountRows() - 1; i >= 0; i--) {
		BRow* row = RowAt(i);
		RemoveRow(row);
		delete row;
	}
}


bool
SearchResultsView::GetSelectedRef(entry_ref* ref) const
{
	ResultRow* row = static_cast<ResultRow*>(CurrentSelection());
	if (row == NULL)
		return false;
	*ref = row->Ref();
	return true;
}


void
SearchResultsView::_StartMonitor()
{
	if (fQuery.Length() == 0)
		return;

	BMessenger indexServer(kIndexServerSignature.String());
	if (!indexServer.IsValid())
		return;

	BMessage request(kMsgStartQueryMonitor);
	request.AddString("query", fQuery);
	request.AddInt32("maxResults", kReplicantMaxResults);
	request.AddMessenger("target", BMessenger(this));

	// Sent with this view as the reply target, so the token comes back
	// through MessageReceived() rather than blocking the window here.
	indexServer.SendMessage(&request, this);
}


void
SearchResultsView::_StopMonitor()
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
}


void
SearchResultsView::_OpenRow(const entry_ref& ref)
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
