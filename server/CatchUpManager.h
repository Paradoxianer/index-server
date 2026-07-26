/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */
#ifndef CATCH_UP_MANAGER_H
#define CATCH_UP_MANAGER_H


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

			//! Add analyser to the queue.
			bool				AddAnalyser(const FileAnalyser* analyser);
			void				RemoveAnalyser(const BString& name);

			//! Spawn a CatchUpAnalyser and fill it with the analyser in the
			//! queue
			bool				CatchUp();
			//! Stop all catch up threads and put the analyser back into the
			//! queue.
			void				Stop();

private:
			BVolume				fVolume;
			IndexServerSettings*	fSettings;

			FileAnalyserList	fFileAnalyserQueue;
			CatchUpAnalyserList	fCatchUpAnalyserList;
};

#endif
