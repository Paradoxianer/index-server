/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef SEARCH_RESULTS_VIEW_H
#define SEARCH_RESULTS_VIEW_H


#include <ColumnListView.h>
#include <String.h>


/*! The search results list (issue #33). The same view serves two roles:

- Inside IndexSearchApp's own window, filled from kMsgQuery replies
  (SearchWindow drives AddResult()/ClearResults()).
- As a Desktop replicant, dragged out of that window via the BDragger
  handle in its corner. The archived copy records only the query; once
  instantiated on the Desktop it registers its own query monitor (issue
  #19) and keeps itself current from the pushed updates.

Follows the Archive()/Instantiate()/BDragger pattern of Haiku replicants
(see src/apps/deskcalc/CalcView.cpp), with the same "add_on" rule: the
executable this class is compiled into provides its code. */
class SearchResultsView : public BColumnListView {
public:
								SearchResultsView(const char* name);
								SearchResultsView(BMessage* archive);
	virtual						~SearchResultsView();

	static	SearchResultsView*	Instantiate(BMessage* archive);
	virtual	status_t			Archive(BMessage* archive,
									bool deep = true) const;

	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				MessageReceived(BMessage* message);

			void				SetQuery(const BString& query);
			const BString&		Query() const;

			void				AddResult(const entry_ref& ref,
									float score);
			void				RemoveResult(const entry_ref& ref);
			void				ClearResults();

			//! False if nothing is selected.
			bool				GetSelectedRef(entry_ref* ref) const;

private:
			void				_Init(bool live);
			void				_StartMonitor();
			void				_StopMonitor();
			void				_OpenRow(const entry_ref& ref);

			BString				fQuery;
			int32				fMonitorToken;
			bool				fLive;
};


#endif // SEARCH_RESULTS_VIEW_H
