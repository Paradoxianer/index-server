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
#include <Locker.h>
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


// Process-wide, not per-volume - each mounted volume gets its own
// CatchUpAnalyser (its own thread), so with several volumes all needing a
// large catch up at once (a tester's system had 4), running every one of
// them's heavy query-and-analyze work concurrently multiplies I/O and CPU
// pressure by however many volumes are involved. Serializing them keeps
// the peak load the same as catching up a single volume, at the cost of
// the total time to work through every volume's backlog (see #33).
BLocker sCatchUpSlot("catch up slot");


// Waits for the slot above, but stays responsive to a stop request while
// waiting - a plain BAutolock would block this looper's thread (and with
// it, its ability to notice Stop()) indefinitely, potentially for as long
// as whichever volume currently holds the slot takes to finish its own
// backlog, which would make shutdown wait on that too.
class CatchUpSlot {
public:
	CatchUpSlot(AnalyserDispatcher* owner)
		:
		fAcquired(false)
	{
		while (sCatchUpSlot.LockWithTimeout(250000) != B_OK) {
			if (owner->Stopped())
				return;
		}
		fAcquired = true;
	}

	~CatchUpSlot()
	{
		if (fAcquired)
			sCatchUpSlot.Unlock();
	}

	bool Acquired() const
	{
		return fAcquired;
	}

private:
	bool fAcquired;
};


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
CatchUpAnalyser::StartAnalysing(bool immediate)
{
	Run();
	BMessage message(kCatchUp);
	BMessageRunner::StartSending(BMessenger(this), &message,
		immediate ? 0 : kCatchUpStartDelay, 1);
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
			&& settings.watchingStart / kSecond <= fEnd) {
			bigtime_t start = system_time();
			analyser->AnalyseEntry(ref);
			bigtime_t elapsed = system_time() - start;
			// See AnalyserDispatcher::AnalyseEntry()'s identical check -
			// this is CatchUpAnalyser's own override, used during an
			// actual catch up run instead of the base class version, so
			// it needs the same instrumentation independently.
			if (elapsed > 200 * 1000) {
				printf("slow analyser (%" B_PRId64 " ms): %s on %s\n",
					elapsed / 1000, analyser->Name().String(), ref.name);
			}
		}
	}
}


void
CatchUpAnalyser::_CatchUp()
{
	fCatchUpManager->PopulateCatchUp(this);

	CatchUpSlot slot(this);
	if (!slot.Acquired()) {
		// Stopped while waiting for another volume's catch up to finish -
		// nothing was queued or committed yet, nothing to clean up.
		return;
	}

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

	// Used to list every covered analyser in the title (e.g. "Indexing
	// Haiku OS (AudioTagAnalyser, ExifAnalyser, ...)") so a user could
	// confirm a specific one was really running - but Haiku's own
	// notification UI truncates a long title instead of wrapping it,
	// which for six-plus analysers just showed "(AudioTagAnalys..." with
	// no way to see the rest. Not worth the length if it can't actually
	// be read; keep the title to just the volume.
	//
	// CatchUpManager only ever runs one CatchUpAnalyser at a time per
	// volume, so the identifier only needs to cover the volume, not the
	// analyser set too - keeping it stable across consecutive runs (one
	// registered after an earlier run's snapshot, a rescan request, ...)
	// means a later run updates the same notification in place instead of
	// its own fading out while a separate one pops up, which looked like
	// the notification randomly disappearing and reappearing.
	BString messageID;
	messageID << "catchup-" << (int32)fVolume.Device();

	BString title;
	title.SetToFormat(B_TRANSLATE("Indexing %s"), volumeName);

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
	fCatchUpPending(false),
	fPendingImmediate(false)
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
				bool immediate = fPendingImmediate;
				fPendingImmediate = false;
				CatchUp(immediate);
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
CatchUpManager::CatchUp(bool immediate)
{
	STRACE("CatchUpManager::CatchUp()\n");
	if (fCatchUpAnalyserList.CountItems() > 0) {
		// Already catching up this volume - let it finish rather than
		// piling another run on top (e.g. a rescan request, see #2,
		// arriving mid catch-up); MessageReceived() starts a follow-up run
		// automatically once it does.
		fCatchUpPending = true;
		fPendingImmediate |= immediate;
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

	catchUpAnalyser->StartAnalysing(immediate);
	return true;
}


bool
CatchUpManager::FullReset(bool immediate)
{
	// PopulateCatchUp() takes the *maximum* sync position across all
	// registered analysers as the run's start - an analyser left behind
	// at an older position would never actually be included (its own
	// syncPosition/kSecond >= fStart check in
	// CatchUpAnalyser::AnalyseEntry() would just fail forever). Resetting
	// every one of them to 0 together keeps them in lock step, so the
	// window genuinely starts from the beginning for all of them instead
	// of excluding whichever one was reset.
	for (size_t i = 0; i < fRegisteredAnalysers.size(); i++) {
		fRegisteredAnalysers[i]->SetSyncPosition(0);
		fRegisteredAnalysers[i]->WriteSettings();
	}
	return CatchUp(immediate);
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
