/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include <AboutWindow.h>
#include <Application.h>

#include "IndexServerPrivate.h"
#include "SettingsWindow.h"


class IndexServerPreflet : public BApplication {
public:
								IndexServerPreflet();

	virtual	void				AboutRequested();

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


void
IndexServerPreflet::AboutRequested()
{
	BAboutWindow* window = new BAboutWindow("Index Server Settings",
		"application/x-vnd.Haiku-IndexServerSettings");
	window->AddDescription(
		"Configures index_server: which paths get indexed (blacklist or "
		"whitelist), and which analysers are enabled.");
	const char* authors[] = {
		"Clemens Zeidler",
		"Matthias Lindner",
		NULL
	};
	const char* extraCopyrights[] = { "2026 Haiku, Inc.", NULL };
	window->AddCopyright(2010, "Clemens Zeidler", extraCopyrights);
	window->AddAuthors(authors);
	window->SetVersion(kIndexServerVersion.String());
	window->Show();
}


int
main()
{
	IndexServerPreflet app;
	app.Run();
	return 0;
}
