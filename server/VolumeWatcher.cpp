/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */

#include "VolumeWatcher.h"

#include <sys/stat.h>

#include <Autolock.h>
#include <Directory.h>
#include <Mime.h>
#include <Node.h>
#include <NodeInfo.h>
#include <NodeMonitor.h>
#include <Path.h>
#include <VolumeRoster.h>
#include <Query.h>
#include <FindDirectory.h>


#include "IndexServerPrivate.h"


const bigtime_t kSecond = 1000000;


WatchNameHandler::WatchNameHandler(VolumeWatcher* volumeWatcher)
	:
	fVolumeWatcher(volumeWatcher)
{

}


void
WatchNameHandler::EntryCreated(const char *name, ino_t directory, dev_t device,
	ino_t node)
{
	entry_ref ref(device, directory, name);
	if (fVolumeWatcher->_IsExcluded(ref))
		return;
	fVolumeWatcher->_RememberEntryRef(ref, node);
	fVolumeWatcher->fCreatedList.CurrentList()->push_back(ref);
	fVolumeWatcher->_NewEntriesArrived();
}


void
WatchNameHandler::EntryRemoved(const char *name, ino_t directory, dev_t device,
	ino_t node)
{
	entry_ref ref(device, directory, name);
	if (fVolumeWatcher->_IsExcluded(ref))
		return;
	fVolumeWatcher->_ForgetEntryRef(node, device);
	fVolumeWatcher->fDeleteList.CurrentList()->push_back(ref);
	fVolumeWatcher->_NewEntriesArrived();
}


void
WatchNameHandler::EntryMoved(const char *name, const char *fromName,
	ino_t from_directory, ino_t to_directory, dev_t device, ino_t node,
	dev_t nodeDevice)
{
	entry_ref ref(device, to_directory, name);
	entry_ref refFrom(device, from_directory, fromName);

	// movedList and movedFromList are paired up by index (see
	// VolumeWorker::_Work), so both sides have to be dropped together.
	if (fVolumeWatcher->_IsExcluded(ref) || fVolumeWatcher->_IsExcluded(refFrom))
		return;

	fVolumeWatcher->_RememberEntryRef(ref, node);
	fVolumeWatcher->fMovedList.CurrentList()->push_back(ref);
	fVolumeWatcher->fMovedFromList.CurrentList()->push_back(refFrom);
	fVolumeWatcher->_NewEntriesArrived();
}


void
WatchNameHandler::StatChanged(ino_t node, dev_t device, int32 statFields)
{
	if ((statFields & B_STAT_MODIFICATION_TIME) == 0)
		return;
}


void
WatchNameHandler::MessageReceived(BMessage* msg)
{
	if (msg->what == B_NODE_MONITOR) {
		int32 opcode;
		if (msg->FindInt32("opcode", &opcode) == B_OK) {
			switch (opcode) {
			case B_STAT_CHANGED: {
				int32 statFields;
				msg->FindInt32("fields", &statFields);
				if ((statFields & B_STAT_MODIFICATION_TIME) == 0)
					break;

				dev_t device;
				ino_t node;
				msg->FindInt32("device", &device);
				msg->FindInt64("node", &node);

				// B_STAT_CHANGED carries only device+node, never a name -
				// see #46. FindEntryRef() resolves it from entries seen
				// via EntryCreated()/EntryMoved() since watching started;
				// nodes never seen that way don't resolve and are dropped.
				entry_ref ref;
				if (!fVolumeWatcher->FindEntryRef(node, device, ref))
					break;

				if (fVolumeWatcher->_IsExcluded(ref))
					break;

				fVolumeWatcher->fModifiedList.CurrentList()->push_back(ref);
				fVolumeWatcher->_NewEntriesArrived();

				break;
			}
			}
		}
	}
	NodeMonitorHandler::MessageReceived(msg);
}


AnalyserDispatcher::AnalyserDispatcher(const char* name)
	:
	BLooper(name, B_LOW_PRIORITY),

	fStopped(0)
{

}


AnalyserDispatcher::~AnalyserDispatcher()
{
	// Every other method touching fFileAnalyserList locks first - this one
	// didn't, racing AddOnMonitorHandler's RemoveAnalyser() during shutdown
	// (add-ons and volumes tearing down concurrently) into a double free.
	BAutolock _(this);
	for (int i = 0; i < fFileAnalyserList.CountItems(); i++)
		delete fFileAnalyserList.ItemAt(i);
}


void
AnalyserDispatcher::Stop()
{
	atomic_set(&fStopped, 1);
}


bool
AnalyserDispatcher::Stopped()
{
	return (atomic_get(&fStopped) != 0);
}


void
AnalyserDispatcher::AnalyseEntry(const entry_ref& ref)
{
	BAutolock _(this);
	_EnsureMimeType(ref);
	for (int i = 0; i < fFileAnalyserList.CountItems(); i++)
		fFileAnalyserList.ItemAt(i)->AnalyseEntry(ref);
}


void
AnalyserDispatcher::DeleteEntry(const entry_ref& ref)
{
	BAutolock _(this);
	for (int i = 0; i < fFileAnalyserList.CountItems(); i++)
		fFileAnalyserList.ItemAt(i)->DeleteEntry(ref);
}


void
AnalyserDispatcher::MoveEntry(const entry_ref& oldRef, const entry_ref& newRef)
{
	BAutolock _(this);
	_EnsureMimeType(newRef);
	for (int i = 0; i < fFileAnalyserList.CountItems(); i++)
		fFileAnalyserList.ItemAt(i)->MoveEntry(oldRef, newRef);
}


// Files that never went through a native Haiku write path (checked out by
// git, arrived over scp, ...) have no BEOS:TYPE at all, which every
// analyser here silently treats as "nothing to do" (see issue #41).
// update_mime_info() with B_UPDATE_MIME_INFO_NO_FORCE only sniffs a type
// when one isn't already set, so this is a no-op for the common case of a
// file Tracker or a native app already typed.
void
AnalyserDispatcher::_EnsureMimeType(const entry_ref& ref)
{
	BNode node(&ref);
	if (node.InitCheck() != B_OK)
		return;

	char type[B_MIME_TYPE_LENGTH];
	BNodeInfo nodeInfo(&node);
	if (nodeInfo.GetType(type) == B_OK)
		return;

	BPath path(&ref);
	if (path.InitCheck() != B_OK)
		return;

	update_mime_info(path.Path(), 0, 1, B_UPDATE_MIME_INFO_NO_FORCE);
}


void
AnalyserDispatcher::LastEntry()
{
	BAutolock _(this);
	for (int i = 0; i < fFileAnalyserList.CountItems(); i++)
		fFileAnalyserList.ItemAt(i)->LastEntry();
}


bool
AnalyserDispatcher::AddAnalyser(FileAnalyser* analyser)
{
	if (analyser == NULL)
		return false;

	bool result;
	BAutolock _(this);
	if (_FindAnalyser(analyser->Name()))
		return false;

	result = fFileAnalyserList.AddItem(analyser);
	return result;
}


bool
AnalyserDispatcher::RemoveAnalyser(const BString& name)
{
	BAutolock _(this);
	FileAnalyser* analyser = _FindAnalyser(name);
	if (analyser) {
		fFileAnalyserList.RemoveItem(analyser);
		delete analyser;
		return true;
	}
	return false;
}


status_t
AnalyserDispatcher::HandleQuery(const BString& analyserName,
	const BMessage& query, BMessage& reply)
{
	bigtime_t lockWaitStart = system_time();
	BAutolock _(this);
	printf("AnalyserDispatcher::HandleQuery: waited %" B_PRId64
		" us for looper lock\n", system_time() - lockWaitStart);
	FileAnalyser* analyser = _FindAnalyser(analyserName);
	if (analyser == NULL)
		return B_NAME_NOT_FOUND;
	bigtime_t queryStart = system_time();
	status_t status = analyser->HandleQuery(query, reply);
	printf("AnalyserDispatcher::HandleQuery: analyser->HandleQuery took %"
		B_PRId64 " us\n", system_time() - queryStart);
	return status;
}


FileAnalyser*
AnalyserDispatcher::_FindAnalyser(const BString& name)
{
	for (int i = 0; i < fFileAnalyserList.CountItems(); i++) {
		FileAnalyser* analyser = fFileAnalyserList.ItemAt(i);
		if (analyser->Name() == name)
			return analyser;
	}
	return NULL;
}


void
AnalyserDispatcher::WriteAnalyserSettings()
{
	for (int i = 0; i < fFileAnalyserList.CountItems(); i++)
		fFileAnalyserList.ItemAt(i)->Settings()->WriteSettings();
}


void
AnalyserDispatcher::SetSyncPosition(bigtime_t time)
{
	for (int i = 0; i < fFileAnalyserList.CountItems(); i++)
		fFileAnalyserList.ItemAt(i)->Settings()->SetSyncPosition(time);
}


void
AnalyserDispatcher::SetWatchingStart(bigtime_t time)
{
	for (int i = 0; i < fFileAnalyserList.CountItems(); i++)
		fFileAnalyserList.ItemAt(i)->Settings()->SetWatchingStart(time);
}


void
AnalyserDispatcher::SetWatchingPosition(bigtime_t time)
{
	for (int i = 0; i < fFileAnalyserList.CountItems(); i++)
		fFileAnalyserList.ItemAt(i)->Settings()->SetWatchingPosition(time);
}


VolumeWorker::VolumeWorker(VolumeWatcher* watcher)
	:
	AnalyserDispatcher("VolumeWorker"),

	fVolumeWatcher(watcher),
	fBusy(0)
{

}


void
VolumeWorker::MessageReceived(BMessage *message)
{
	switch (message->what) {
		case kTriggerWork:
			_Work();
			break;

		default:
			BLooper::MessageReceived(message);
	}
}


bool
VolumeWorker::IsBusy()
{
	return (atomic_get(&fBusy) != 0);
}


void
VolumeWorker::_Work()
{
	list_collection collection;
	fVolumeWatcher->GetSecureEntries(collection);

	if (collection.createdList->size() == 0
		&& collection.deletedList->size() == 0
		&& collection.modifiedList->size() == 0
		&& collection.movedList->size() == 0)
		return;

	_SetBusy(true);
	for (unsigned int i = 0; i < collection.createdList->size() || Stopped();
		i++)
		AnalyseEntry((*collection.createdList)[i]);
	collection.createdList->clear();

	for (unsigned int i = 0; i < collection.deletedList->size() || Stopped();
		i++)
		DeleteEntry((*collection.deletedList)[i]);
	collection.deletedList->clear();

	for (unsigned int i = 0; i < collection.modifiedList->size() || Stopped();
		i++)
		AnalyseEntry((*collection.modifiedList)[i]);
	collection.modifiedList->clear();

	for (unsigned int i = 0; i < collection.movedList->size() || Stopped();
		i++)
		MoveEntry((*collection.movedFromList)[i], (*collection.movedList)[i]);
	collection.movedList->clear();
	collection.movedFromList->clear();

	LastEntry();
	PostMessage(kTriggerWork);

	_SetBusy(false);
}


void
VolumeWorker::_SetBusy(bool busy)
{
	if (busy)
		atomic_set(&fBusy, 1);
	else
		atomic_set(&fBusy, 0);
}


VolumeWatcherBase::VolumeWatcherBase(const BVolume& volume)
	:
	fVolume(volume),
	fEnabled(true),
	fLastUpdated(0)
{
	ReadSettings();
}


const char* kEnabledAttr = "Enabled";


bool
VolumeWatcherBase::ReadSettings()
{
	BVolume bootVolume;
	BVolumeRoster roster;
	char volumenName[255];
	roster.GetBootVolume(&bootVolume);
	if (bootVolume == fVolume) {
		fEnabled = true;
		WriteSettings();
	}
	
	fVolume.GetName(volumenName);
	BPath path;
	find_directory(B_USER_SETTINGS_DIRECTORY,&path);
	
	path.Append(kIndexServerDirectory);
	path.Append(volumenName);
	path.Append(kVolumeStatusFileName);
	BFile file(path.Path(), B_READ_ONLY);
	if (file.InitCheck() != B_OK)
		return false;

	uint32 enabled;
	file.WriteAttr(kEnabledAttr, B_UINT32_TYPE, 0, &enabled, sizeof(uint32));
	fEnabled = enabled == 0 ? false : true;
	return true;
}


bool
VolumeWatcherBase::WriteSettings()
{
	BPath path;
	char volumenName[255];
	find_directory(B_USER_SETTINGS_DIRECTORY,&path);
	path.Append(kIndexServerDirectory);
	if (create_directory(path.Path(), 777) != B_OK)
		return false;
	fVolume.GetName(volumenName);
	path.Append(volumenName);
	if (create_directory(path.Path(), 777) != B_OK)
		return false;
	path.Append(kVolumeStatusFileName);
	BFile file(path.Path(), B_READ_WRITE | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() != B_OK)
		return false;

	uint32 enabled = fEnabled ? 1 : 0;
	file.WriteAttr(kEnabledAttr, B_UINT32_TYPE, 0, &enabled, sizeof(uint32));
	return true;
}


SwapEntryRefVector::SwapEntryRefVector()
{
	fCurrentList = &fFirstList;
	fNextList = &fSecondList;
}


EntryRefVector*
SwapEntryRefVector::SwapList()
{
	EntryRefVector* temp = fCurrentList;
	fCurrentList = fNextList;
	fNextList = temp;
	return temp;
}


EntryRefVector*
SwapEntryRefVector::CurrentList()
{
	return fCurrentList;
}


VolumeWatcher::VolumeWatcher(const BVolume& volume,
	IndexServerSettings* settings)
	:
	VolumeWatcherBase(volume),
	BLooper("VolumeWatcher"),

	fWatching(false),
	fSettings(settings),
	fWatchNameHandler(this),
	fCatchUpManager(volume, settings)
{
	AddHandler(&fWatchNameHandler);
	// CatchUpAnalyser messages fCatchUpManager (a plain BHandler, not a
	// BLooper) via a BMessenger built from just the handler pointer; that
	// only resolves if the handler already belongs to a looper. Without
	// this, kCatchUpDone silently never arrives (BMessenger ends up
	// invalid) and every finished CatchUpAnalyser leaks instead of being
	// cleaned up - true since the original 2010 code, just never
	// noticeable until CatchUpManager::CatchUp() started relying on
	// kCatchUpDone to know when it's safe to start the next run.
	AddHandler(&fCatchUpManager);

	fVolumeWorker = new VolumeWorker(this);
	fVolumeWorker->Run();
}


VolumeWatcher::~VolumeWatcher()
{
	Stop();
	thread_id threadId = fVolumeWorker->Thread();
	fVolumeWorker->PostMessage(B_QUIT_REQUESTED);
	status_t error;
	wait_for_thread(threadId, &error);
}


bool
VolumeWatcher::StartWatching()
{
	Run();

	watch_volume(fVolume.Device(), B_WATCH_NAME | B_WATCH_STAT,
		&fWatchNameHandler);

	// set the time after start watching to not miss anything
	fVolumeWorker->SetWatchingStart(real_time_clock_usecs());

	char name[255];
	fVolume.GetName(name);

	fCatchUpManager.CatchUp();

	fWatching = true;
	return true;
}


void
VolumeWatcher::Stop()
{

	char name[255];
	fVolume.GetName(name);

	// set the time before stop watching to not miss anything
	fVolumeWorker->SetWatchingPosition(real_time_clock_usecs());

	stop_watching(&fWatchNameHandler);

	fVolumeWorker->WriteAnalyserSettings();

	// don't stop the work because we have to handle all entries after writing
	// the watching position
	//fVolumeWorker->Stop();
	fCatchUpManager.Stop();
}


bool
VolumeWatcher::AddAnalyser(FileAnalyser* analyser)
{
	if (!fVolumeWorker->AddAnalyser(analyser))
		return false;

	BAutolock _(this);
	if (!fCatchUpManager.AddAnalyser(analyser))
		return false;

	if (fWatching)
		fCatchUpManager.CatchUp();

	return true;
}


bool
VolumeWatcher::RemoveAnalyser(const BString& name)
{
	if (!fVolumeWorker->RemoveAnalyser(name))
		return false;

	BAutolock _(this);
	fCatchUpManager.RemoveAnalyser(name);
	return true;
}


void
VolumeWatcher::RequestRescan(const BString& name)
{
	BAutolock _(this);
	printf("VolumeWatcher::RequestRescan requested by %s\n", name.String());
	fCatchUpManager.CatchUp();
}


void
VolumeWatcher::RequestFullReset()
{
	BAutolock _(this);
	printf("VolumeWatcher::RequestFullReset\n");
	fCatchUpManager.FullReset();
}


status_t
VolumeWatcher::HandleQuery(const BString& analyserName,
	const BMessage& query, BMessage& reply)
{
	return fVolumeWorker->HandleQuery(analyserName, query, reply);
}


bool
VolumeWatcher::IsBusy()
{
	BAutolock _(this);
	return fVolumeWorker->IsBusy() || fCatchUpManager.IsCatchingUp();
}


void
VolumeWatcher::GetSecureEntries(list_collection& collection)
{
	BAutolock _(this);
	collection.createdList = fCreatedList.SwapList();
	collection.deletedList = fDeleteList.SwapList();
	collection.modifiedList = fModifiedList.SwapList();
	collection.movedList = fMovedList.SwapList();
	collection.movedFromList = fMovedFromList.SwapList();
}


bool
VolumeWatcher::FindEntryRef(ino_t node, dev_t device, entry_ref& entry)
{
	BAutolock _(this);
	NodeRefMap::iterator it = fEntryRefMap.find(node_ref(device, node));
	if (it == fEntryRefMap.end())
		return false;
	entry = it->second;
	return true;
}


void
VolumeWatcher::_RememberEntryRef(const entry_ref& ref, ino_t node)
{
	BAutolock _(this);
	fEntryRefMap[node_ref(ref.device, node)] = ref;
}


void
VolumeWatcher::_ForgetEntryRef(ino_t node, dev_t device)
{
	BAutolock _(this);
	fEntryRefMap.erase(node_ref(device, node));
}


void
VolumeWatcher::_NewEntriesArrived()
{
	// The fVolumeWorker has to exist as long as we live so directly post to
	// the queue.
	if (fVolumeWorker->IsBusy())
		return;
	fVolumeWorker->PostMessage(kTriggerWork);
}


bool
VolumeWatcher::_IsExcluded(const entry_ref& ref) const
{
	if (fSettings == NULL)
		return false;

	BPath path(&ref);
	if (path.InitCheck() != B_OK)
		return false;

	return fSettings->IsPathExcluded(BString(path.Path()), fVolume);
}
