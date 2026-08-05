/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include <stdio.h>

#include <Application.h>

#include "SearchWindow.h"


class IndexSearchApp : public BApplication {
public:
								IndexSearchApp();

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
