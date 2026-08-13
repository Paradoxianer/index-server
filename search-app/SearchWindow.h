/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef SEARCH_WINDOW_H
#define SEARCH_WINDOW_H


#include <Window.h>


class BButton;
class BColumnListView;
class BMessageRunner;
class BTextControl;
class BStringView;


class SearchWindow : public BWindow {
public:
								SearchWindow();
	virtual						~SearchWindow();

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

private:
			void				_RunSearch();
			void				_LoadMore();
			void				_SendQuery(int32 offset);
			void				_ScheduleLiveFilter();
			void				_HandleQueryReply(BMessage* reply);
			void				_OpenSelected();
			void				_UpdateLoadMoreButton();

			BTextControl*		fQueryControl;
			BColumnListView*	fResultsView;
			BButton*			fLoadMoreButton;
			BStringView*		fStatusView;
			bigtime_t			fSearchSentTime;
			BMessageRunner*		fFilterRunner;
			int32				fPendingQueryToken;
			int32				fCurrentOffset;
			int32				fTotalHits;
};


#endif // SEARCH_WINDOW_H
