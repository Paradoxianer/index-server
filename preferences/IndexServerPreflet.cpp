/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include <Application.h>

#include "SettingsWindow.h"


class IndexServerPreflet : public BApplication {
public:
								IndexServerPreflet();

private:
			SettingsWindow*		fWindow;
};


IndexServerPreflet::IndexServerPreflet()
	:
	BApplication("application/x-vnd.Haiku-IndexServerSettings")
{
	fWindow = new SettingsWindow();
	fWindow->Show();
}


int
main()
{
	IndexServerPreflet app;
	app.Run();
	return 0;
}
