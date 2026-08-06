/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */

#include "IndexServer.h"

#include <locale.h>
#include <stdio.h>


int
main()
{
	// Redirected to a log file, stdout is fully buffered rather than
	// line-buffered; for a long-running server that makes its log useless
	// for anything but a post-mortem read after a clean exit.
	setvbuf(stdout, NULL, _IOLBF, 0);

	// Haiku paths/filenames are UTF-8 natively, but a process starts in the
	// "POSIX" locale by default, which mbstowcs() (used by to_wchar() in
	// CLuceneDataBase.cpp to build every CLucene field/path) can't decode
	// multi-byte sequences in - any path containing so much as one accented
	// character then silently fails to convert, and the whole document
	// never gets indexed at all. Only LC_CTYPE matters here (byte/wide-char
	// conversion rules) - leaving other categories alone avoids any
	// unrelated locale-dependent formatting changes elsewhere.
	setlocale(LC_CTYPE, "en_US.UTF-8");

	IndexServer indexServer;
	indexServer.Run();
	return 0;
}
