/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include <stdio.h>

#include <AboutWindow.h>
#include <Application.h>

#include "IndexServerPrivate.h"
#include "SearchWindow.h"


class IndexSearchApp : public BApplication {
public:
								IndexSearchApp();

	virtual	void				AboutRequested();

private:
			SearchWindow*		fWindow;
};


IndexSearchApp::IndexSearchApp()
	:
	BApplication("application/x-vnd.Haiku-IndexServerSearch")
{
	fWindow = new SearchWindow();
	fWindow->Show();
}


void
IndexSearchApp::AboutRequested()
{
	BAboutWindow* window = new BAboutWindow("Index Search",
		"application/x-vnd.Haiku-IndexServerSearch");
	window->AddDescription(
		"Searches the full-text index kept live by index_server. Full-text "
		"content isn't something Tracker's own queries can see, since it "
		"lives in a separate index outside BFS - this window is what "
		"checks it directly.");
	// Unlike index_server itself, this search window has no predecessor -
	// it's new code written for this project's revival, not inherited from
	// Ankur Sethi's 2009 original or Clemens Zeidler's 2010 GSoC rewrite
	// (see IndexServer.cpp's AboutRequested() for that lineage).
	const char* authors[] = {
		"Matthias Lindner",
		NULL
	};
	window->AddCopyright(2026, "Haiku, Inc.");
	window->AddAuthors(authors);
	window->SetVersion(kIndexServerVersion.String());
	window->Show();
}


int
main()
{
	// Without this, stdout is fully buffered once redirected to a file
	// (not line-buffered like a terminal), so debug output in
	// SearchWindow.cpp would only appear once the app quits.
	setvbuf(stdout, NULL, _IOLBF, 0);

	IndexSearchApp app;
	app.Run();
	return 0;
}
