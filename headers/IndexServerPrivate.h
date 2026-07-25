/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */
#ifndef INDEX_SERVER_PRIVATE_H
#define INDEX_SERVER_PRIVATE_H


#include <String.h>


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
