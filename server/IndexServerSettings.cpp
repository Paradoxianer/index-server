/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "IndexServerSettings.h"

#include <Autolock.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <NodeMonitor.h>

#include "IndexServerPrivate.h"


#define DEBUG_INDEX_SERVER_SETTINGS
#ifdef DEBUG_INDEX_SERVER_SETTINGS
#include <stdio.h>
#	define STRACE(x...) printf("IndexServerSettings: " x)
#else
#	define STRACE(x...) ;
#endif


static const char* kModeField = "mode";
static const char* kPathsField = "paths";


IndexServerSettings::IndexServerSettings()
	:
	BHandler("IndexServerSettings"),
	fMode(kBlacklistMode),
	fWatchingSettings(false)
{
	_SetDefaults();
	if (!_Load())
		_Save();
}


IndexServerSettings::~IndexServerSettings()
{
	StopWatchingSettings();
}


void
IndexServerSettings::_SetDefaults()
{
	BAutolock lock(fLock);

	fMode = kBlacklistMode;
	fPaths.clear();

	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK) {
		path.Append(kIndexServerDirectory);
		fPaths.push_back(BString(path.Path()));
	}
	if (find_directory(B_SYSTEM_CACHE_DIRECTORY, &path) == B_OK)
		fPaths.push_back(BString(path.Path()));
	if (find_directory(B_SYSTEM_LOG_DIRECTORY, &path) == B_OK)
		fPaths.push_back(BString(path.Path()));
	if (find_directory(B_TRASH_DIRECTORY, &path) == B_OK)
		fPaths.push_back(BString(path.Path()));
	if (find_directory(B_SYSTEM_PACKAGES_DIRECTORY, &path) == B_OK)
		fPaths.push_back(BString(path.Path()));
}


BPath
IndexServerSettings::_SettingsFilePath() const
{
	BPath path;
	find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	path.Append(kIndexServerDirectory);
	path.Append(kSettingsFileName);
	return path;
}


bool
IndexServerSettings::_Load()
{
	BPath path = _SettingsFilePath();
	BFile file(path.Path(), B_READ_ONLY);
	if (file.InitCheck() != B_OK)
		return false;

	BMessage settings;
	if (settings.Unflatten(&file) != B_OK)
		return false;

	BAutolock lock(fLock);

	int32 mode;
	if (settings.FindInt32(kModeField, &mode) == B_OK)
		fMode = mode == kWhitelistMode ? kWhitelistMode : kBlacklistMode;

	BString path_;
	fPaths.clear();
	for (int32 i = 0; settings.FindString(kPathsField, i, &path_) == B_OK; i++)
		fPaths.push_back(path_);

	STRACE("loaded %ld exclude paths, mode %d\n", (long)fPaths.size(),
		fMode);
	return true;
}


bool
IndexServerSettings::_Save()
{
	BPath dirPath;
	find_directory(B_USER_SETTINGS_DIRECTORY, &dirPath);
	dirPath.Append(kIndexServerDirectory);
	if (create_directory(dirPath.Path(), 0777) != B_OK)
		return false;

	BAutolock lock(fLock);

	BMessage settings;
	settings.AddInt32(kModeField, fMode);
	for (size_t i = 0; i < fPaths.size(); i++)
		settings.AddString(kPathsField, fPaths[i]);

	lock.Unlock();

	BPath path = _SettingsFilePath();
	BFile file(path.Path(), B_READ_WRITE | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() != B_OK)
		return false;

	return settings.Flatten(&file) == B_OK;
}


void
IndexServerSettings::StartWatchingSettings()
{
	if (fWatchingSettings)
		return;

	BPath path = _SettingsFilePath();
	BEntry entry(path.Path());
	entry_ref ref;
	if (entry.GetRef(&ref) != B_OK)
		return;

	BNode node(&entry);
	if (node.GetNodeRef(&fSettingsNodeRef) != B_OK)
		return;

	if (watch_node(&fSettingsNodeRef, B_WATCH_STAT, this) != B_OK)
		return;

	fWatchingSettings = true;
}


void
IndexServerSettings::StopWatchingSettings()
{
	if (!fWatchingSettings)
		return;

	stop_watching(this);
	fWatchingSettings = false;
}


void
IndexServerSettings::MessageReceived(BMessage* message)
{
	if (message->what != B_NODE_MONITOR) {
		BHandler::MessageReceived(message);
		return;
	}

	int32 opcode;
	if (message->FindInt32("opcode", &opcode) != B_OK || opcode != B_STAT_CHANGED)
		return;

	STRACE("settings file changed, reloading\n");
	_Load();
}


bool
IndexServerSettings::IsPathExcluded(const BString& path, const BVolume& volume) const
{
	// The per-volume analyser data (e.g. the CLucene index) always lives
	// under <volume root>/index_server. Excluding it here, independent of
	// the configurable path list below, is what breaks the feedback loop:
	// the server would otherwise re-analyse the files it just wrote.
	BDirectory rootDir;
	if (volume.GetRootDirectory(&rootDir) == B_OK) {
		BPath volumeIndexDir(&rootDir);
		volumeIndexDir.Append(kIndexServerDirectory);
		if (path_is_or_is_under(path, BString(volumeIndexDir.Path())))
			return true;
	}

	BAutolock lock(fLock);
	bool listed = false;
	for (size_t i = 0; i < fPaths.size(); i++) {
		if (path_is_or_is_under(path, fPaths[i])) {
			listed = true;
			break;
		}
	}
	return fMode == kBlacklistMode ? listed : !listed;
}
