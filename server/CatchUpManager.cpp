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

	if (entryList.size() == 0)
		return;

	// A routine catch up after a short restart is over in a blink; only a
	// backlog large enough to actually take a while is worth telling the
	// user about (see issue #34).
	const size_t kNotifyThreshold = 200;
	IndexProgressNotifier* notifier = NULL;
	if (entryList.size() >= kNotifyThreshold)
		notifier = _CreateProgressNotifier();

	for (uint32 i = 0; i < entryList.size(); i++) {
		if (Stopped()) {
			// Commit whatever was queued so far - otherwise interrupting a
			// large catch up (e.g. server restart before it finishes) would
			// silently discard all progress made up to this point.
			LastEntry();
			delete notifier;
			return;
		}
		if (i % 100 == 0) {
			printf("Catch up: %i/%i\n", (int)i,(int)entryList.size());
			if (notifier != NULL)
				notifier->Progress(i, entryList.size());
		}
		AnalyseEntry(entryList[i]);
		snooze(kCatchUpPaceInterval);
		if (i % 500 == 0 && i > 0)
			LastEntry();
	}
	LastEntry();

	if (notifier != NULL) {
		notifier->Done(entryList.size());
		delete notifier;
	}

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

	// Analysers are named after their add-on (e.g. "FullTextAnalyser"); a
	// volume can be catching up more than one at once (see
	// CatchUpManager::CatchUp(), called once per registering add-on), each
	// as its own CatchUpAnalyser, so the identifier needs to cover both the
	// volume and the analyser set to not collide with a sibling catch up.
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

	return new IndexProgressNotifier(messageID, title);
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
	fSettings(settings)
{

}


CatchUpManager::~CatchUpManager()
{
	Stop();

	for (int i = 0; i < fFileAnalyserQueue.CountItems(); i++)
		delete fFileAnalyserQueue.ItemAt(i);
}


void
CatchUpManager::MessageReceived(BMessage *message)
{
	CatchUpAnalyser* analyser = NULL;
	switch (message->what) {
		case kCatchUpDone:
			message->GetPointer("Analyser", &analyser);
			fCatchUpAnalyserList.RemoveItem(analyser);
			analyser->PostMessage(B_QUIT_REQUESTED);
		break;

		default:
			BHandler::MessageReceived(message);
	}
}


bool
CatchUpManager::AddAnalyser(const FileAnalyser* analyserOrg)
{
	IndexServer* server = (IndexServer*)be_app;
	FileAnalyser* analyser = server->CreateFileAnalyser(analyserOrg->Name(),
		fVolume);
	if (!analyser)
		return false;
	ASSERT(analyserOrg->Settings());
	analyser->SetSettings(analyserOrg->Settings());

	bool status = fFileAnalyserQueue.AddItem(analyser);
	if (!status)
		delete analyser;
	return status;
}


void
CatchUpManager::RemoveAnalyser(const BString& name)
{
	for (int i = 0; i < fFileAnalyserQueue.CountItems(); i++) {
		FileAnalyser* analyser = fFileAnalyserQueue.ItemAt(i);
		if (analyser->Name() == name) {
			fFileAnalyserQueue.RemoveItem(analyser);
			delete analyser;
		}
	}

	for (int i = 0; i < fCatchUpAnalyserList.CountItems(); i++)
		fCatchUpAnalyserList.ItemAt(i)->RemoveAnalyser(name);
}


bool
CatchUpManager::CatchUp()
{
	STRACE("CatchUpManager::CatchUp()\n");
	bigtime_t startBig  = 0;
	bigtime_t endBig = real_time_clock_usecs();
	for (int i = 0; i < fFileAnalyserQueue.CountItems(); i++) {
		FileAnalyser* analyser = fFileAnalyserQueue.ItemAt(i);
 		analyser->UpdateSettingsCache();
		const analyser_settings& settings = analyser->CachedSettings();
		STRACE("%s, %i, %i\n", analyser->Name().String(),
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

	for (int i = 0; i < fFileAnalyserQueue.CountItems(); i++) {
		FileAnalyser* analyser = fFileAnalyserQueue.ItemAt(i);
		// if AddAnalyser fails at least don't leak
		if (!catchUpAnalyser->AddAnalyser(analyser))
			delete analyser;
			
	}
	fFileAnalyserQueue.MakeEmpty();

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
