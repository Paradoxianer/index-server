/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef INDEX_SERVER_SETTINGS_H
#define INDEX_SERVER_SETTINGS_H


#include <vector>

#include <Handler.h>
#include <Locker.h>
#include <Node.h>
#include <Path.h>
#include <String.h>
#include <Volume.h>


enum settings_mode {
	kBlacklistMode = 0,
	kWhitelistMode = 1
};


typedef std::vector<BString> PathList;


/*! Holds the exclude/include path list and reloads it live when the
settings file changes. Queried by every VolumeWatcher and CatchUpAnalyser,
so all accessors are thread safe. */
class IndexServerSettings : public BHandler {
public:
								IndexServerSettings();
	virtual						~IndexServerSettings();

			void				StartWatchingSettings();
			void				StopWatchingSettings();

	virtual	void				MessageReceived(BMessage* message);

			//! thread safe
			bool				IsPathExcluded(const BString& path,
									const BVolume& volume) const;

private:
			void				_SetDefaults();
			bool				_Load();
			bool				_Save();
			BPath				_SettingsFilePath() const;

	mutable	BLocker				fLock;
			settings_mode		fMode;
			PathList			fPaths;

			node_ref			fSettingsNodeRef;
			bool				fWatchingSettings;
};


#endif // INDEX_SERVER_SETTINGS_H
