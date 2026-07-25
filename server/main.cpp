/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */

#include "IndexServer.h"

#include <stdio.h>


int
main()
{
	// Redirected to a log file, stdout is fully buffered rather than
	// line-buffered; for a long-running server that makes its log useless
	// for anything but a post-mortem read after a clean exit.
	setvbuf(stdout, NULL, _IOLBF, 0);

	IndexServer indexServer;
	indexServer.Run();
	return 0;
}
