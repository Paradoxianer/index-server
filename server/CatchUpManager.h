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


class CatchUpAnalyser : public AnalyserDispatcher {
public:
								CatchUpAnalyser(const BVolume& volume,
									time_t start, time_t end,
									BHandler* manager,
									IndexServerSettings* settings);

			void				MessageReceived(BMessage *message);
			void				StartAnalysing();

			void				AnalyseEntry(const entry_ref& ref);

			const BVolume&		Volume() { return fVolume; }

private:
			void				_CatchUp();
			void				_WriteSyncSatus(bigtime_t syncTime);
			IndexProgressNotifier*	_CreateProgressNotifier();

			BVolume				fVolume;
			time_t				fStart;
			time_t				fEnd;

			BHandler*			fCatchUpManager;
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
			//! dropped.
			bool				CatchUp();
			//! Stop all catch up threads.
			void				Stop();

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
};

#endif
