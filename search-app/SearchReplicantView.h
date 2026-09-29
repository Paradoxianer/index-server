/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef SEARCH_REPLICANT_VIEW_H
#define SEARCH_REPLICANT_VIEW_H


#include <map>

#include <String.h>
#include <View.h>


/*! A live search result as a Desktop replicant (issue #33) - drag the
BDragger handle SearchWindow embeds out onto the Desktop (or any other
BShelf, e.g. Deskbar's), and this keeps showing the current matches for
its query indefinitely, updated live via the query monitor API (issue
#19) rather than re-polling on a timer.

Follows the same Archive()/Instantiate()/BDragger pattern every real
Haiku replicant uses (see src/apps/deskcalc/CalcView.cpp for the
reference this was modelled on): "add_on" in the archive is this view's
own executable's app signature (IndexServerSearch's, since that's where
this class's code lives) - Haiku's instantiate_object() loads that
executable as an add-on purely to pull the class's code from it,
without ever running its main()/Run(), the same way it does for
DeskCalc's own corner-dragger replicant. */
class SearchReplicantView : public BView {
public:
								SearchReplicantView(BRect frame,
									const char* query);
								SearchReplicantView(BMessage* archive);
	virtual						~SearchReplicantView();

	static	SearchReplicantView*	Instantiate(BMessage* archive);
	virtual	status_t			Archive(BMessage* archive,
									bool deep = true) const;

	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);

			//! Changes which query this replicant follows - re-registers
			//! its query monitor from scratch. Used by SearchWindow's own
			//! embedded preview instance to track whatever the user is
			//! currently typing; a replicant already dropped onto the
			//! Desktop has no UI to call this from, so it keeps
			//! following whatever query it was created or restored with.
			void				SetQuery(const BString& query);

private:
			void				_Init();
			void				_StartMonitor();
			void				_StopMonitor();
			void				_OpenMatch(const entry_ref& ref);

			BString				fQuery;
			int32				fMonitorToken;
				//! The full current match set, keyed by path (see
				//! IndexServer::QueryMonitor's own lastMatches for why
				//! path rather than entry_ref) - kMsgQueryMonitorUpdate
				//! only ever carries what changed, so this is what Draw()
				//! actually renders from.
			std::map<BString, entry_ref>	fMatches;
};


#endif // SEARCH_REPLICANT_VIEW_H
