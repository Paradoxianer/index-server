/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef SETTINGS_WINDOW_H
#define SETTINGS_WINDOW_H


#include <Window.h>

#include "IndexServerSettings.h"


class BFilePanel;
class BMenu;
class BMenuBar;
class BMenuField;
class PathListView;


class SettingsWindow : public BWindow {
public:
								SettingsWindow();
	virtual						~SettingsWindow();

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

private:
			void				_BuildAnalysersMenu(BMenuBar* menuBar);
			void				_ReloadPathList();
			void				_AddRefs(BMessage* message);
			void				_RemoveSelectedPaths();

			IndexServerSettings	fSettings;

			BMenuField*			fModeField;
			BMenu*				fAnalysersMenu;
			PathListView*		fPathListView;
			BFilePanel*			fFolderPanel;
};


#endif // SETTINGS_WINDOW_H
