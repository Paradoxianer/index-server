/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */
#ifndef CATCH_UP_MANAGER_H
#define CATCH_UP_MANAGER_H


#include <vector>

#include "AnalyserDispatcher.h"
#include "IndexServerSettings.h"


#define DEBUG_CATCH_UP
#ifdef DEBUG_CATCH_UP
#include <stdio.h>
#	define STRACE(x...) printf(x)
#else
#	define STRACE(x...) ;
#endif


class IndexProgressNotifier;
class CatchUpManager;


class CatchUpAnalyser : public AnalyserDispatcher {
public:
								CatchUpAnalyser(const BVolume& volume,
									CatchUpManager* manager,
									IndexServerSettings* settings);

			void				MessageReceived(BMessage *message);
			//! immediate skips kCatchUpStartDelay - for a run the user
			//! explicitly asked for right now (a rescan/full reset request),
			//! as opposed to one following naturally from add-on/volume
			//! registration at startup, which the delay exists for.
			void				StartAnalysing(bool immediate = false);

			//! Called by CatchUpManager::PopulateCatchUp(), right before
			//! the delayed kCatchUp message fires - see its own comment
			//! for why this isn't done any earlier.
			void				SetTimeRange(time_t start, time_t end);

			void				AnalyseEntry(const entry_ref& ref);

			const BVolume&		Volume() { return fVolume; }

private:
			void				_CatchUp();
			void				_WriteSyncSatus(bigtime_t syncTime);
			IndexProgressNotifier*	_CreateProgressNotifier();

			BVolume				fVolume;
			time_t				fStart;
			time_t				fEnd;

			CatchUpManager*		fCatchUpManager;
			IndexServerSettings*	fSettings;
};


typedef BObjectList<CatchUpAnalyser> CatchUpAnalyserList;


class CatchUpManager : public BHandler {
public:
								CatchUpManager(const BVolume& volume,
									IndexServerSettings* settings);
								~CatchUpManager();

			void				MessageReceived(BMessage *message);

			//! Register an analyser as wanting to take part in catch up.
			//! Keeps a reference to its settings, not the analyser itself -
			//! CatchUp() may run more than once over this object's lifetime
			//! (e.g. FileAnalyser::RequestRescan()) and builds a fresh
			//! catch-up clone from the add-on each time.
			bool				AddAnalyser(const FileAnalyser* analyser);
			void				RemoveAnalyser(const BString& name);

			//! Spawn a CatchUpAnalyser covering every registered analyser.
			//! If one is already running for this volume, this instead
			//! marks a catch up as pending: it starts automatically as
			//! soon as the current run finishes, so a registration or
			//! rescan request that arrives mid-run is never silently
			//! dropped. immediate skips kCatchUpStartDelay (see
			//! StartAnalysing()) - if a run is already pending and this
			//! one asks for immediate, that preference carries over to
			//! the deferred run too.
			bool				CatchUp(bool immediate = false);
			//! Resets every registered analyser's sync position back to
			//! the start and calls CatchUp() - unlike a normal CatchUp()
			//! alone, this forces every one of them to be reindexed from
			//! scratch rather than just picking up genuinely new changes
			//! since their last sync position. See kMsgRequestFullReset.
			bool				FullReset(bool immediate = false);
			//! Stop all catch up threads.
			void				Stop();

			//! Clones every currently registered analyser into
			//! catchUpAnalyser and computes its time window - called from
			//! CatchUpAnalyser::_CatchUp() itself, not from CatchUp(): at
			//! CatchUp()-call time, only whichever analyser triggered this
			//! particular run (e.g. the first one to register at startup)
			//! is necessarily registered yet - others arriving during the
			//! kCatchUpStartDelay pause would each just mark themselves
			//! pending and get their own, separate, later run instead of
			//! joining this one. Deferring this until just before the
			//! query actually runs picks up everyone who registered by
			//! then instead.
			void				PopulateCatchUp(
									CatchUpAnalyser* catchUpAnalyser);

			//! True if a catch up run is currently active for this volume.
			bool				IsCatchingUp()
									{ return fCatchUpAnalyserList.CountItems()
										> 0; }

private:
			BVolume				fVolume;
			IndexServerSettings*	fSettings;

			std::vector<BReference<AnalyserSettings> >	fRegisteredAnalysers;
			CatchUpAnalyserList	fCatchUpAnalyserList;
			bool				fCatchUpPending;
			//! Whether the pending run (above) should skip
			//! kCatchUpStartDelay once it actually starts - sticky once
			//! set, since a second CatchUp(true) arriving while one's
			//! already pending shouldn't be forgotten just because the
			//! first one that set fCatchUpPending didn't ask for it.
			bool				fPendingImmediate;
};

#endif
