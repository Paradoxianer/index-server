/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */

#include "IndexServer.h"

#include <stdio.h>
#include <string.h>

#include <FindDirectory.h>
#include <Path.h>

#include "IndexServerPrivate.h"


// Started as a launch_daemon service, this has no terminal and no log
// unless something explicitly redirects stdout (e.g. dev.sh's
// start-logged) - meaning a tester hitting a problem has nothing to send
// back. Log to a fixed, discoverable location every run instead, no setup
// required on their end.
static void
_StartLogging()
{
	BPath path;
	if (find_directory(B_USER_LOG_DIRECTORY, &path) != B_OK)
		return;
	if (path.Append(kIndexServerDirectory.String()) != B_OK)
		return;
	if (create_directory(path.Path(), 0755) != B_OK)
		return;
	if (path.Append("index_server.log") != B_OK)
		return;

	// Keep one previous run's log around too - the one that matters after
	// a crash-and-restart is often the run before the current one, not the
	// few lines since the restart.
	BString oldLogPath(path.Path());
	oldLogPath << ".old";
	remove(oldLogPath.String());
	rename(path.Path(), oldLogPath.String());

	freopen(path.Path(), "w", stdout);
}


int
main()
{
	_StartLogging();

	// Redirected to a log file, stdout is fully buffered rather than
	// line-buffered; for a long-running server that makes its log useless
	// for anything but a post-mortem read after a clean exit.
	setvbuf(stdout, NULL, _IOLBF, 0);

	// BServer's constructor takes error by out-parameter instead of
	// silently exit(0)-ing on failure the way a plain, no-error-param
	// BApplication(signature) would - see the constructor's own comment
	// in IndexServer.cpp for why that distinction matters here. Log it
	// rather than letting it vanish the same way.
	status_t error = B_OK;
	IndexServer indexServer(error);
	if (error != B_OK) {
		printf("IndexServer failed to start: %s\n", strerror(error));
		return 1;
	}

	indexServer.Run();
	return 0;
}
