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

#endif // INDEX_SERVER_PRIVATE_H
