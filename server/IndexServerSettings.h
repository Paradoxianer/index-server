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

			//! thread safe; true unless explicitly disabled
			bool				IsAnalyserEnabled(const BString& name) const;

			// The accessors below are for the settings preflet, which runs
			// as its own instance in its own team (never sharing an
			// IndexServerSettings object with the server), so they don't
			// need to be as carefully thread safe as the exclude-path/
			// analyser-enabled checks above, which race against a live
			// node-monitor reload.
			settings_mode		Mode() const;
			void				SetMode(settings_mode mode);

			PathList			Paths() const;
			void				AddPath(const BString& path);
			void				RemovePath(const BString& path);

			PathList			DisabledAnalysers() const;
			void				SetAnalyserEnabled(const BString& name,
									bool enabled);

			void				Save();
			void				ResetToDefaults();
			//! Re-reads the settings file, discarding any in-memory state
			//! that wasn't saved (there shouldn't be any, since every
			//! accessor above saves immediately, but the file may have
			//! changed from outside this instance).
			void				Reload();

private:
			void				_SetDefaults();
			bool				_Load();
			bool				_Save();
			BPath				_SettingsFilePath() const;

	mutable	BLocker				fLock;
			settings_mode		fMode;
			PathList			fPaths;
			PathList			fDisabledAnalysers;

			node_ref			fSettingsNodeRef;
			bool				fWatchingSettings;
};


#endif // INDEX_SERVER_SETTINGS_H
