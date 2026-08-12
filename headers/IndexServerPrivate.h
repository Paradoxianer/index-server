/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */
#ifndef INDEX_SERVER_PRIVATE_H
#define INDEX_SERVER_PRIVATE_H


#include <Directory.h>
#include <FindDirectory.h>
#include <Path.h>
#include <String.h>
#include <Volume.h>
#include <VolumeRoster.h>


// Must match IndexServer's BApplication() signature (IndexServer.cpp) and
// index_server.rdef's app_signature resource.
const BString kIndexServerSignature = "application/x-vnd.Haiku-index_server";

const BString kIndexServerDirectory = "index_server";
const BString kVolumeStatusFileName = "VolumeStatus";
const BString kSettingsFileName = "settings";


/*! True if \a path is equal to \a prefix or lies below it. Both paths are
expected to be normalized (no trailing slash, no "..").  */
inline bool
path_is_or_is_under(const BString& path, const BString& prefix)
{
	if (path == prefix)
		return true;
	if (path.Length() <= prefix.Length())
		return false;
	if (path.Compare(prefix, prefix.Length()) != 0)
		return false;
	return path[prefix.Length()] == '/';
}


/*! Where per-volume analyser data (e.g. the CLucene index) lives for
\a volume. Packagefs reserves the boot volume's top level for "system",
"home" etc., so a bare "index_server" folder there doesn't belong; use the
regenerable-data location Haiku already provides instead. Other volumes
have no such system-wide location to hang per-volume data off of, so it
still has to sit at that volume's own root - but hidden (dot-prefixed),
not as a folder Tracker shows by default. */
inline BPath
volume_index_server_directory(const BVolume& volume)
{
	BVolume bootVolume;
	BVolumeRoster().GetBootVolume(&bootVolume);

	BPath path;
	if (volume == bootVolume) {
		find_directory(B_SYSTEM_CACHE_DIRECTORY, &path);
		path.Append(kIndexServerDirectory.String());
	} else {
		BDirectory rootDir;
		volume.GetRootDirectory(&rootDir);
		path.SetTo(&rootDir);
		BString hiddenName(".");
		hiddenName += kIndexServerDirectory;
		path.Append(hiddenName.String());
	}
	return path;
}

// messages between preferences app
const uint32 kStopWatching = 'StoW';
const uint32 kStartWatching = 'StaW';

const uint32 kRegisterWatcher = 'RegW';
const uint32 kVolumenAdded = 'VAdd';
const uint32 kVolumenRemoved = 'VRem';
const uint32 kAddOnAdded = 'AAdd';
const uint32 kAddOnRemoved = 'ARem';

const uint32 kGetVolumenInfos = 'GVIn';
const uint32 kGetAddOnInfos = 'GAIn';

const uint32 kEnableAddOn = 'EnaA';
const uint32 kDisableAddOn = 'DisA';

//! Sent to the index_server BApplication to run a content search; carries
//! "query" (BString) and optionally "maxResults" (int32, default 100).
//! Send this asynchronously with a replyTo handler (not the synchronous
//! two-way SendMessage()) - a search can take a while if it has to wait
//! for a CLucene lock held by an in-progress Commit(), and a caller
//! blocked waiting for the reply blocks its own window's message loop
//! for that whole time. Answered via BMessage::SendReply() with "what"
//! set to kMsgQueryReply and repeated "refs" (entry_ref) and "scores"
//! (float, same order) fields, plus "searchedVolumes" (int32).
const uint32 kMsgQuery = 'ISQu';

//! "what" of kMsgQuery's reply - see kMsgQuery.
const uint32 kMsgQueryReply = 'ISQR';

//! Name of the analyser add-on that answers kMsgQuery (matches the add-on
//! binary's own file name, which FileAnalyser::Name() is set from - see
//! IndexServer::RegisterAddOn()).
const BString kFullTextAnalyserName = "FullTextAnalyser";

//! Sent to the index_server BApplication to ask whether it's currently
//! idle or catching up/indexing. No fields needed in the request. Send
//! this asynchronously with a replyTo handler - see kMsgQuery's comment
//! for why. Answered via BMessage::SendReply() with "what" set to
//! kMsgGetStatusReply and "indexing" (bool) - true if any watched volume
//! has a catch up or live analysis run in progress.
const uint32 kMsgGetStatus = 'ISSt';

//! "what" of kMsgGetStatus's reply - see kMsgGetStatus.
const uint32 kMsgGetStatusReply = 'ISSR';

//! Sent to the index_server BApplication to force a complete reindex on
//! every watched volume: resets every registered analyser's sync
//! position back to the start and re-runs catch up from scratch. Unlike
//! a normal catch up (which only picks up genuinely new changes since
//! each analyser's own last sync position), this is for recovering after
//! an analyser's own external index was wiped or corrupted in a way the
//! sync position alone doesn't reflect (e.g. FullTextAnalyser's CLucene
//! data directory) - the sync position would otherwise claim old content
//! is already covered when nothing on disk actually backs that anymore.
//! No fields needed in the request; no reply is sent.
const uint32 kMsgRequestFullReset = 'ISFR';

//! Sent to the index_server BApplication to subscribe to live catch up
//! progress pushes (see kMsgIndexProgress below); carries "observer"
//! (BMessenger) - the specific handler to push updates to, not just the
//! sending team's default one. Registering the same target twice is a
//! no-op (deduped via BMessenger::operator==).
const uint32 kMsgRegisterProgressObserver = 'ISRO';

//! Unregisters a target previously registered with
//! kMsgRegisterProgressObserver (same "observer" field) - send when a
//! window with a progress display closes, so the server stops wasting a
//! SendMessage() on a dead target every update.
const uint32 kMsgUnregisterProgressObserver = 'ISUO';

//! Pushed by index_server to every registered progress observer during a
//! catch up run. Carries "current" (int32), "total" (int32), "volume"
//! (BString, human-readable volume name) and "path" (BString, the file
//! just analysed - empty for the final "done" push, where current==total).
const uint32 kMsgIndexProgress = 'ISPr';

#endif // INDEX_SERVER_PRIVATE_H
