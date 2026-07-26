/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
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
	IndexSearchApp app;
	app.Run();
	return 0;
}
