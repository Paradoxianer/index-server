/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "SearchWindow.h"

#include <vector>

#include <Application.h>
#include <Button.h>
#include <Catalog.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <Entry.h>
#include <LayoutBuilder.h>
#include <Path.h>
#include <Roster.h>
#include <StringView.h>
#include <TextControl.h>
#include <Volume.h>
#include <VolumeRoster.h>

#include "CLuceneSearcher.h"
#include "FullTextAnalyser.h"
#include "IndexServerPrivate.h"


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
	if (queryString.Length() == 0) {
		fStatusView->SetText(B_TRANSLATE("Type something to search for."));
		return;
	}

	std::vector<search_result> results;

	BVolumeRoster volumeRoster;
	BVolume volume;
	int32 searchedVolumes = 0;
	while (volumeRoster.GetNextVolume(&volume) == B_OK) {
		if (volume.Capacity() <= 0 || !volume.KnowsQuery())
			continue;

		BPath indexPath = volume_index_server_directory(volume);
		indexPath.Append(kFullTextDirectory);

		CLuceneSearcher searcher(indexPath);
		if (!searcher.IndexExists())
			continue;
		searchedVolumes++;
		searcher.Search(queryString, results);
	}

	for (size_t i = 0; i < results.size(); i++) {
		BRow* row = new BRow();
		row->SetField(new BStringField(results[i].path.String()),
			kPathColumn);
		BString scoreText;
		scoreText.SetToFormat("%.2f", results[i].score);
		row->SetField(new BStringField(scoreText.String()), kScoreColumn);
		fResultsView->AddRow(row);
	}

	BString status;
	if (searchedVolumes == 0) {
		status = B_TRANSLATE("No volume has a full text index yet.");
	} else {
		status.SetToFormat(
			B_TRANSLATE("%ld result(s) across %ld indexed volume(s)."),
			(long)results.size(), (long)searchedVolumes);
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
