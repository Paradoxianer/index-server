/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */
#ifndef INDEX_SERVER_H
#define INDEX_SERVER_H


#include <map>
#include <utility>
#include <vector>

#include <MessageRunner.h>
#include <Messenger.h>
#include <Server.h>
#include <String.h>
#include <VolumeRoster.h>

#include <AddOnMonitorHandler.h>
#include <ObjectList.h>

#include "IndexServerAddOn.h"
#include "IndexServerSettings.h"
#include "VolumeWatcher.h"


#define DEBUG_INDEX_SERVER
#ifdef DEBUG_INDEX_SERVER
#include <stdio.h>
#	define STRACE(x...) printf(x)
#else
#	define STRACE(x...) ;
#endif


class IndexServer;


//! One persistent query registered via kMsgStartQueryMonitor (issue #19).
//! lastMatches is keyed by path rather than entry_ref - entry_ref has no
//! ordering operator to use as a std::map key - but still holds each
//! match's actual entry_ref (not just its score) so a match that
//! disappears (its file deleted) can still be reported as removed via
//! the entry_ref last seen for it, without needing to re-resolve a path
//! that may no longer exist on disk by the time of that recheck.
struct QueryMonitor {
			int32				token;
			BString				query;
			int32				maxResults;
			BMessenger			target;
			std::map<BString, std::pair<entry_ref, float> >	lastMatches;
};


class VolumeObserverHandler : public BHandler {
public:
								VolumeObserverHandler(IndexServer* indexServer);
			void				MessageReceived(BMessage *message);
private:
			IndexServer*		fIndexServer;
};


class AnalyserMonitorHandler : public AddOnMonitorHandler {
public:
								AnalyserMonitorHandler(
									IndexServer* indexServer);

private:
			void				AddOnEnabled(
									const add_on_entry_info* entryInfo);
			void				AddOnDisabled(
									const add_on_entry_info* entryInfo);

			IndexServer*		fIndexServer;
};


class IndexServer : public BServer {
public:
								IndexServer(status_t& error);
	virtual						~IndexServer();

	virtual void				ReadyToRun();
	virtual	void				MessageReceived(BMessage *message);

	virtual	bool				QuitRequested();
	virtual	void				AboutRequested();

			void				AddVolume(const BVolume& volume);
			void				RemoveVolume(const BVolume& volume);

			void				RegisterAddOn(entry_ref ref);
			void				UnregisterAddOn(entry_ref ref);

			//! thread safe
			FileAnalyser*		CreateFileAnalyser(const BString& name,
									const BVolume& volume);
private:
			void				_StartWatchingVolumes();
			void				_StopWatchingVolumes();

			void				_SetupVolumeWatcher(VolumeWatcher* watcher);
			FileAnalyser*		_SetupFileAnalyser(IndexServerAddOn* addon,
									const BVolume& volume);
			void				_StartWatchingAddOns();

	inline	IndexServerAddOn*	_FindAddon(const BString& name);
				//! Shared teardown for one addon: detach it from every
				//! volume watcher, unload its image, delete it. Used by
				//! both UnregisterAddOn() and RegisterAddOn() (to retire a
				//! same-named entry before adding a new one - see #14).
				void				_RetireAddOn(IndexServerAddOn* addon);

				//! Shared by kMsgQuery and every query monitor recheck -
				//! runs \a queryString across every watched volume and
				//! merges the results into \a reply the same way
				//! kMsgQueryReply is shaped (see IndexServerPrivate.h).
				void				_RunQuery(const BString& queryString,
										int32 maxResults, int32 offset,
										BMessage& reply);

				//! Reruns every registered query monitor's query and
				//! pushes a kMsgQueryMonitorUpdate for whichever ones
				//! actually gained or lost a match since their own
				//! lastMatches. Drops a monitor outright if its target
				//! has quit (SendMessage() failing is the only way to
				//! tell). Called from the kMsgIndexContentChanged handler
				//! and the query monitor pulse, never more than once per
				//! kQueryMonitorPulseInterval either way (see
				//! fQueryMonitorsDirty's comment).
				void				_RecheckQueryMonitors();

			BVolumeRoster		fVolumeRoster;
			BObjectList<VolumeWatcher>		fVolumeWatcherList;
			BObjectList<IndexServerAddOn>	fAddOnList;

			IndexServerSettings	fSettings;

			VolumeObserverHandler	fVolumeObserverHandler;

			AnalyserMonitorHandler	fAddOnMonitorHandler;
			BMessageRunner*			fPulseRunner;

			std::vector<QueryMonitor>	fQueryMonitors;
			int32				fNextQueryMonitorToken;
				//! Set by the kMsgIndexContentChanged handler (and by
				//! AddVolume()/RemoveVolume(), which can change results
				//! without any single file changing); cleared once
				//! _RecheckQueryMonitors() actually runs. A dirty flag
				//! plus a periodic pulse - rather than rechecking
				//! straight from kMsgIndexContentChanged - coalesces a
				//! whole catch up's worth of per-batch notifications
				//! (VolumeWorker::_Work() posts one after every batch,
				//! which can be many in a row for a large backlog) into
				//! one recheck instead of rerunning every monitor's query
				//! that many times over.
			bool				fQueryMonitorsDirty;
			BMessageRunner*			fQueryMonitorPulseRunner;
};


#endif
