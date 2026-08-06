/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */

#include "CatchUpManager.h"

#include <algorithm>
#include <vector>

#include <Autolock.h>
#include <Catalog.h>
#include <Debug.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <Node.h>
#include <Path.h>
#include <Query.h>

#include "IndexProgressNotifier.h"
#include "IndexServer.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "CatchUpManager"


const uint32 kCatchUp = '&CaU';
const uint32 kCatchUpDone = '&CUD';

const bigtime_t kSecond = 1000000;

// Boot/login is when the disk and CPU are busiest with everything else
// starting up too; giving the first catch up some time to get out of the
// way avoids piling straight onto that (see #33).
const bigtime_t kCatchUpStartDelay = 20 * kSecond;

// A short breather between files during a large catch up, so a bulk scan
// doesn't monopolize the disk against whatever the user is doing at the
// same time. A first-pass heuristic, not a measured optimum - see #33.
const bigtime_t kCatchUpPaceInterval = 500;

// How often (in processed entries) to advance and persist the sync
// position mid-run - the same cadence LastEntry() already commits at, so
// a crash or restart only ever has to redo up to this many entries
// instead of the entire backlog (see #48).
const uint32 kSyncPositionInterval = 500;


namespace {


// entryList is sorted by modified time before processing so the sync
// position, once advanced past an entry, is a genuine guarantee that
// everything with an earlier modification time has been committed - the
// BQuery itself has no defined result order to rely on for that.
struct QueueEntry {
	entry_ref	ref;
	time_t		modified;
};


bool
CompareQueueEntry(const QueueEntry& a, const QueueEntry& b)
{
	return a.modified < b.modified;
}


}	// namespace


CatchUpAnalyser::CatchUpAnalyser(const BVolume& volume,
	CatchUpManager* manager, IndexServerSettings* settings)
	:
	AnalyserDispatcher("CatchUpAnalyser"),
	fVolume(volume),
	fStart(0),
	fEnd(0),
	fCatchUpManager(manager),
	fSettings(settings)
{

}


void
CatchUpAnalyser::SetTimeRange(time_t start, time_t end)
{
	fStart = start;
	fEnd = end;
}


void
CatchUpAnalyser::MessageReceived(BMessage *message)
{
	switch (message->what) {
		case kCatchUp:
			_CatchUp();
		break;

		default:
			BLooper::MessageReceived(message);
	}
}


void
CatchUpAnalyser::StartAnalysing()
{
	Run();
	BMessage message(kCatchUp);
	BMessageRunner::StartSending(BMessenger(this), &message,
		kCatchUpStartDelay, 1);
}


void
CatchUpAnalyser::AnalyseEntry(const entry_ref& ref)
{
	BAutolock _(this);
	_EnsureMimeType(ref);
	for (int i = 0; i < fFileAnalyserList.CountItems(); i++) {
		FileAnalyser* analyser = fFileAnalyserList.ItemAt(i);
		const analyser_settings& settings = analyser->CachedSettings();
		if (settings.syncPosition / kSecond >= fStart
			&& settings.watchingStart / kSecond <= fEnd)
			analyser->AnalyseEntry(ref);
	}
}


void
CatchUpAnalyser::_CatchUp()
{
	fCatchUpManager->PopulateCatchUp(this);

	STRACE("_CatchUp start %i, end %i\n", (int)fStart, (int)fEnd);
	for (int i = 0; i < fFileAnalyserList.CountItems(); i++)
		STRACE("- Analyser %s\n", fFileAnalyserList.ItemAt(i)->Name().String());

	BQuery query;
	query.SetVolume(&fVolume);
	query.PushAttr("last_modified");
	query.PushInt32(fStart);
	query.PushOp(B_GE);
	query.PushAttr("last_modified");
	query.PushInt32(fEnd);
	query.PushOp(B_LE);
	query.PushOp(B_AND);

	query.Fetch();
	
	int32  pLength = query.PredicateLength();
	char predicate[pLength+1];
	query.GetPredicate(predicate,pLength);
	PRINT(("catchup query: %s \n",predicate));
	
	std::vector<QueueEntry> entryList;
	entry_ref ref;
	while (query.GetNextRef(&ref) == B_OK) {
		if (fSettings != NULL) {
			BPath path(&ref);
			if (path.InitCheck() == B_OK
				&& fSettings->IsPathExcluded(BString(path.Path()), fVolume)) {
				continue;
			}
		}
		QueueEntry entry;
		entry.ref = ref;
		entry.modified = 0;
		BNode node(&ref);
		node.GetModificationTime(&entry.modified);
		entryList.push_back(entry);
	}

	// A BQuery's result order isn't defined, but advancing the sync
	// position mid-run needs a guarantee that everything with an earlier
	// modification time really has been committed already (see #48) -
	// sort once up front instead of relying on incidental query order.
	std::sort(entryList.begin(), entryList.end(), CompareQueueEntry);

	PRINT(("CatchUpAnalyser:: entryList.size() %i\n", (int)entryList.size()));

	// Deliberately not an early return on an empty entryList: this still
	// needs to fall through to advance the sync position and send
	// kCatchUpDone below. A CatchUpAnalyser that returns without ever
	// sending kCatchUpDone stays in CatchUpManager's list forever, which
	// (now that only one catch up runs at a time per volume, see
	// CatchUpManager::CatchUp()) would permanently block every later catch
	// up for this volume, including any pending one.

	// Always created (a Settings window watching live progress wants an
	// update whether the backlog is big or small); IndexProgressNotifier
	// itself decides whether a run is big enough to also pop up an OS
	// notification (see its own kNotifyThreshold).
	IndexProgressNotifier* notifier = _CreateProgressNotifier();

	for (uint32 i = 0; i < entryList.size(); i++) {
		if (Stopped()) {
			// Commit whatever was queued so far, and advance the sync
			// position to match what was actually committed - otherwise
			// interrupting a large catch up (e.g. server restart before it
			// finishes) would discard all progress made up to this point,
			// not just the content (already safe - see #48's history) but
			// the resume point too, forcing the entire backlog to be
			// redone from scratch on the next run.
			LastEntry();
			if (i > 0) {
				_WriteSyncSatus(
					(bigtime_t)entryList[i - 1].modified * kSecond);
			}
			delete notifier;
			return;
		}
		if (i % 100 == 0)
			printf("Catch up: %i/%i\n", (int)i,(int)entryList.size());
		// Progress() throttles itself to at most once a second (always
		// letting the first/last update through), so calling it every
		// iteration is what lets a quick catch up (fewer than 100 entries,
		// otherwise never hitting the printf's own gate above) still push
		// at least a start and an end update to observers.
		notifier->Progress(i, entryList.size(), BPath(&entryList[i].ref).Path());
		AnalyseEntry(entryList[i].ref);
		snooze(kCatchUpPaceInterval);
		if (i % kSyncPositionInterval == 0 && i > 0) {
			LastEntry();
			_WriteSyncSatus((bigtime_t)entryList[i].modified * kSecond);
		}
	}
	LastEntry();

	notifier->Done(entryList.size());
	delete notifier;

	_WriteSyncSatus(fEnd * kSecond);
	printf(("Catched up.\n"));

	BMessenger managerMessenger(fCatchUpManager);
	BMessage msg(kCatchUpDone);
	msg.AddPointer("Analyser", this);
	managerMessenger.SendMessage(&msg);
}


IndexProgressNotifier*
CatchUpAnalyser::_CreateProgressNotifier()
{
	char volumeName[B_FILE_NAME_LENGTH];
	if (fVolume.GetName(volumeName) != B_OK)
		strlcpy(volumeName, "?", sizeof(volumeName));

	// Analysers are named after their add-on (e.g. "FullTextAnalyser").
	// Naming them here is what lets a user actually confirm a specific
	// analyser (e.g. AudioTagAnalyser) is really running rather than
	// silently sitting idle - worth the length. #53 fixed the underlying
	// problem this used to expose (the startup run routinely covered only
	// one analyser, whichever registered first) rather than papering over
	// it by hiding the analyser list here.
	BString analyserNames;
	for (int i = 0; i < fFileAnalyserList.CountItems(); i++) {
		if (i > 0)
			analyserNames << ", ";
		analyserNames << fFileAnalyserList.ItemAt(i)->Name();
	}

	// CatchUpManager only ever runs one CatchUpAnalyser at a time per
	// volume, but consecutive runs can still cover different analyser sets
	// (one registered after the previous run's snapshot was taken), so the
	// identifier still needs to cover both the volume and the analyser set
	// to not collide with an earlier run's still-fading notification.
	BString messageID;
	messageID << "catchup-" << (int32)fVolume.Device() << "-"
		<< analyserNames;

	BString title;
	title.SetToFormat(B_TRANSLATE("Indexing %s (%s)"), volumeName,
		analyserNames.String());

	return new IndexProgressNotifier(messageID, title, volumeName, fSettings);
}


void
CatchUpAnalyser::_WriteSyncSatus(bigtime_t syncTime)
{
	for (int i = 0; i < fFileAnalyserList.CountItems(); i++) {
		AnalyserSettings* settings = fFileAnalyserList.ItemAt(i)->Settings();
		ASSERT(settings);
		settings->SetSyncPosition(syncTime);
		settings->WriteSettings();
	}
	
}


CatchUpManager::CatchUpManager(const BVolume& volume,
	IndexServerSettings* settings)
	:
	fVolume(volume),
	fSettings(settings),
	fCatchUpPending(false)
{

}


CatchUpManager::~CatchUpManager()
{
	Stop();
}


void
CatchUpManager::MessageReceived(BMessage *message)
{
	CatchUpAnalyser* analyser = NULL;
	switch (message->what) {
		case kCatchUpDone:
			// GetPointer(name, defaultValue) is the "return with a
			// fallback" convenience overload, not FindPointer's
			// status_t/out-param pair - passing &analyser here filled the
			// unused defaultValue parameter and silently discarded the
			// actual return, leaving analyser NULL forever. That left a
			// finished CatchUpAnalyser stuck in the list, so every later
			// CatchUp() (e.g. a rescan) saw "already running" and just
			// re-marked itself pending, never actually running again.
			message->FindPointer("Analyser", (void**)&analyser);
			fCatchUpAnalyserList.RemoveItem(analyser);
			if (analyser != NULL)
				analyser->PostMessage(B_QUIT_REQUESTED);

			if (fCatchUpPending) {
				// Something registered or asked for a rescan while this run
				// was still going; it was folded into fRegisteredAnalysers
				// already but missed this run's snapshot, so give it its
				// own run now instead of leaving it stranded.
				fCatchUpPending = false;
				CatchUp();
			}
		break;

		default:
			BHandler::MessageReceived(message);
	}
}


bool
CatchUpManager::AddAnalyser(const FileAnalyser* analyserOrg)
{
	ASSERT(analyserOrg->Settings());
	fRegisteredAnalysers.push_back(
		BReference<AnalyserSettings>(analyserOrg->Settings()));
	return true;
}


void
CatchUpManager::RemoveAnalyser(const BString& name)
{
	for (size_t i = 0; i < fRegisteredAnalysers.size(); i++) {
		if (fRegisteredAnalysers[i]->Name() == name) {
			fRegisteredAnalysers.erase(fRegisteredAnalysers.begin() + i);
			break;
		}
	}

	for (int i = 0; i < fCatchUpAnalyserList.CountItems(); i++)
		fCatchUpAnalyserList.ItemAt(i)->RemoveAnalyser(name);
}


bool
CatchUpManager::CatchUp()
{
	STRACE("CatchUpManager::CatchUp()\n");
	if (fCatchUpAnalyserList.CountItems() > 0) {
		// Already catching up this volume - let it finish rather than
		// piling another run on top (e.g. a rescan request, see #2,
		// arriving mid catch-up); MessageReceived() starts a follow-up run
		// automatically once it does.
		fCatchUpPending = true;
		return false;
	}
	if (fRegisteredAnalysers.empty())
		return false;

	// Which analysers this run actually covers, and its time window, are
	// filled in later by PopulateCatchUp() - see its own comment for why.
	CatchUpAnalyser* catchUpAnalyser = new CatchUpAnalyser(fVolume, this,
		fSettings);
	if (!catchUpAnalyser)
		return false;
	if (!fCatchUpAnalyserList.AddItem(catchUpAnalyser)) {
		delete catchUpAnalyser;
		return false;
	}

	catchUpAnalyser->StartAnalysing();
	return true;
}


void
CatchUpManager::PopulateCatchUp(CatchUpAnalyser* catchUpAnalyser)
{
	bigtime_t startBig  = 0;
	bigtime_t endBig = real_time_clock_usecs();
	for (size_t i = 0; i < fRegisteredAnalysers.size(); i++) {
		analyser_settings settings = fRegisteredAnalysers[i]->RawSettings();
		STRACE("%s, %i, %i\n", fRegisteredAnalysers[i]->Name().String(),
			  (int)settings.syncPosition, (int)settings.watchingStart);
		if (startBig < settings.syncPosition )
			startBig = settings.syncPosition;
		if (settings.watchingStart > endBig)
			endBig = settings.watchingStart;
	}
	catchUpAnalyser->SetTimeRange(startBig / kSecond, endBig / kSecond);

	// Each catch up run gets its own fresh FileAnalyser clone from the
	// add-on - the registered AnalyserSettings reference is what survives
	// across runs, not the analyser object itself.
	IndexServer* server = (IndexServer*)be_app;
	for (size_t i = 0; i < fRegisteredAnalysers.size(); i++) {
		FileAnalyser* analyser = server->CreateFileAnalyser(
			fRegisteredAnalysers[i]->Name(), fVolume);
		if (analyser == NULL)
			continue;
		analyser->SetSettings(fRegisteredAnalysers[i].Get());
		if (!catchUpAnalyser->AddAnalyser(analyser))
			delete analyser;
	}
}


void
CatchUpManager::Stop()
{
	for (int i = 0; i < fCatchUpAnalyserList.CountItems(); i++) {
		CatchUpAnalyser* catchUpAnalyser = fCatchUpAnalyserList.ItemAt(i);
		catchUpAnalyser->Stop();
		catchUpAnalyser->PostMessage(B_QUIT_REQUESTED);
	}
	fCatchUpAnalyserList.MakeEmpty();
}
