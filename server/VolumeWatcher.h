/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */
#ifndef VOLUME_WATCHER_H
#define VOLUME_WATCHER_H


#include <vector>

#include <Debug.h>
#include <Handler.h>
#include <NodeMonitorHandler.h>
#include <Volume.h>

#include <ObjectList.h>

#include "AnalyserDispatcher.h"
#include "CatchUpManager.h"
#include "IndexServerAddOn.h"
#include "IndexServerSettings.h"


class VolumeWatcher;


class WatchNameHandler : public NodeMonitorHandler {
public:
								WatchNameHandler(VolumeWatcher* volumeWatcher);

			void				EntryCreated(const char *name, ino_t directory,
									dev_t device, ino_t node);
			void				EntryRemoved(const char *name, ino_t directory,
									dev_t device, ino_t node);
			void				EntryMoved(const char *name,
									const char *fromName, ino_t from_directory,
									ino_t to_directory, dev_t device,
									ino_t node, dev_t nodeDevice);
			void				StatChanged(ino_t node, dev_t device,
									int32 statFields);

			void				MessageReceived(BMessage* msg);
private:
			VolumeWatcher*		fVolumeWatcher;
};


typedef std::vector<entry_ref> EntryRefVector;


class VolumeWatcher;


const uint32 kTriggerWork = '&twk';	// what a bad message


class VolumeWorker : public AnalyserDispatcher
{
public:
								VolumeWorker(VolumeWatcher* watcher);

			void				MessageReceived(BMessage *message);

			bool				IsBusy();

private:
			void				_Work();

			void				_SetBusy(bool busy = true);

			VolumeWatcher*		fVolumeWatcher;
			int32				fBusy;
};


class VolumeWatcherBase {
public:
								VolumeWatcherBase(const BVolume& volume);

			const BVolume&		Volume() { return fVolume; }

			bool				Enabled() { return fEnabled; }
			bigtime_t			GetLastUpdated() { return fLastUpdated; }

protected:
			bool				ReadSettings();
			bool				WriteSettings();

			BVolume				fVolume;

			bool				fEnabled;

			bigtime_t			fLastUpdated;
};


/*! Used to thread safe exchange refs. While the watcher thread file the current
list the worker thread can handle the second list. The worker thread gets his entries by calling SwapList while holding the watcher thread lock. */
class SwapEntryRefVector {
public:
								SwapEntryRefVector();

			EntryRefVector*		SwapList();
			EntryRefVector*		CurrentList();
private:
			EntryRefVector		fFirstList;
			EntryRefVector		fSecondList;
			EntryRefVector*		fCurrentList;
			EntryRefVector*		fNextList;
};


struct list_collection
{
			EntryRefVector*		createdList;
			EntryRefVector*		deletedList;
			EntryRefVector*		modifiedList;
			EntryRefVector*		movedList;
			EntryRefVector*		movedFromList;
};


/*! Watch a volume and delegate changed entries to a VolumeWorker. */
class VolumeWatcher : public VolumeWatcherBase, public BLooper {
public:
								VolumeWatcher(const BVolume& volume,
									IndexServerSettings* settings);
								~VolumeWatcher();

			bool				StartWatching();
			void				Stop();

			//! thread safe
			bool				AddAnalyser(FileAnalyser* analyser);
			bool				RemoveAnalyser(const BString& name);
			//! Re-run catch up for every registered analyser on this
			//! volume; no-ops if one is already in progress. See
			//! FileAnalyser::RequestRescan().
			void				RequestRescan(const BString& name);

			//! thread safe
			status_t		HandleQuery(const BString& analyserName,
								const BMessage& query,
								BMessage& reply);

			void				GetSecureEntries(list_collection& collection);

			bool				FindEntryRef(ino_t node, dev_t device,
									entry_ref& entry);

private:
	friend class WatchNameHandler;

			void				_NewEntriesArrived();

			//! true if ref should not be handed to the AnalyserDispatcher
			bool				_IsExcluded(const entry_ref& ref) const;

			bool				fWatching;

			IndexServerSettings*	fSettings;

			WatchNameHandler	fWatchNameHandler;

			SwapEntryRefVector	fCreatedList;
			SwapEntryRefVector	fDeleteList;
			SwapEntryRefVector	fModifiedList;
			SwapEntryRefVector	fMovedList;
			SwapEntryRefVector	fMovedFromList;

			VolumeWorker*		fVolumeWorker;
			CatchUpManager		fCatchUpManager;
};


#endif
