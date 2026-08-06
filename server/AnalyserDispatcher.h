/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */
#ifndef ANALYSER_DISPATCHER
#define ANALYSER_DISPATCHER


#include <Looper.h>
#include <Message.h>
#include <String.h>

#include "IndexServerAddOn.h"


class FileAnalyser;


class AnalyserDispatcher : public BLooper {
public:
								AnalyserDispatcher(const char* name);
								~AnalyserDispatcher();

			void				Stop();
			bool				Stopped();

			bool				Busy();

			void				AnalyseEntry(const entry_ref& ref);
			void				DeleteEntry(const entry_ref& ref);
			void				MoveEntry(const entry_ref& oldRef,
									const entry_ref& newRef);
			void				LastEntry();

			//! thread safe
			bool				AddAnalyser(FileAnalyser* analyser);
			bool				RemoveAnalyser(const BString& name);

			//! thread safe
			status_t			HandleQuery(const BString& analyserName,
									const BMessage& query, BMessage& reply);

			void				WriteAnalyserSettings();
			void				SetSyncPosition(bigtime_t time);
			void				SetWatchingStart(bigtime_t time);
			void				SetWatchingPosition(bigtime_t time);

protected:
			FileAnalyserList	fFileAnalyserList;

			//! Files that never went through a native Haiku write path
			//! (checked out by git, arrived over scp, ...) have no
			//! BEOS:TYPE at all, which every analyser here silently treats
			//! as "nothing to do" (see #41). Every AnalyseEntry()/
			//! MoveEntry() path needs this before dispatching -
			//! CatchUpAnalyser used to skip it entirely, silently leaving
			//! an untyped file invisible to catch up forever (see #50).
			void				_EnsureMimeType(const entry_ref& ref);

private:
			FileAnalyser*		_FindAnalyser(const BString& name);

			int32				fStopped;
};

#endif // ANALYSER_DISPATCHER
