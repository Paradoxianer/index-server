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
			void				_ScheduleLiveFilter();
			void				_HandleQueryReply(BMessage* reply);
			void				_OpenSelected();

			BTextControl*		fQueryControl;
			BColumnListView*	fResultsView;
			BStringView*		fStatusView;
			bigtime_t			fSearchSentTime;
			BMessageRunner*		fFilterRunner;
			int32				fPendingQueryToken;
};


#endif // SEARCH_WINDOW_H
