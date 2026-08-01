/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */

#include "CatchUpManager.h"

#include <vector>

#include <Autolock.h>
#include <Catalog.h>
#include <Debug.h>
#include <MessageRunner.h>
#include <Messenger.h>
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


CatchUpAnalyser::CatchUpAnalyser(const BVolume& volume, time_t start,
	time_t end, BHandler* manager, IndexServerSettings* settings)
	:
	AnalyserDispatcher("CatchUpAnalyser"),
	fVolume(volume),
	fStart(start),
	fEnd(end),
	fCatchUpManager(manager),
	fSettings(settings)
{

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
	
	std::vector<entry_ref> entryList;
	entry_ref ref;
	while (query.GetNextRef(&ref) == B_OK) {
		if (fSettings != NULL) {
			BPath path(&ref);
			if (path.InitCheck() == B_OK
				&& fSettings->IsPathExcluded(BString(path.Path()), fVolume)) {
				continue;
			}
		}
		entryList.push_back(ref);
	}

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
			// Commit whatever was queued so far - otherwise interrupting a
			// large catch up (e.g. server restart before it finishes) would
			// silently discard all progress made up to this point.
			LastEntry();
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
		notifier->Progress(i, entryList.size(), BPath(&entryList[i]).Path());
		AnalyseEntry(entryList[i]);
		snooze(kCatchUpPaceInterval);
		if (i % 500 == 0 && i > 0)
			LastEntry();
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
	// CatchUpManager only ever runs one CatchUpAnalyser at a time per
	// volume now, but consecutive runs can cover different analyser sets
	// (one registered after the previous run's snapshot was taken), so the
	// identifier still needs to cover both the volume and the analyser set
	// to not collide with an earlier run's still-fading notification.
	BString analyserNames;
	for (int i = 0; i < fFileAnalyserList.CountItems(); i++) {
		if (i > 0)
			analyserNames << ", ";
		analyserNames << fFileAnalyserList.ItemAt(i)->Name();
	}

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

	CatchUpAnalyser* catchUpAnalyser = new CatchUpAnalyser(fVolume,
		startBig / kSecond, endBig / kSecond, this, fSettings);
	if (!catchUpAnalyser)
		return false;
	if (!fCatchUpAnalyserList.AddItem(catchUpAnalyser)) {
		delete catchUpAnalyser;
		return false;
	}

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

	catchUpAnalyser->StartAnalysing();
	return true;
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
