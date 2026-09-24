/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef RUN_TRANSLATOR_HELPER_H
#define RUN_TRANSLATOR_HELPER_H


#include <OS.h>


/*! Runs IndexServerTranslateHelper (translate-helper/TranslateHelper.cpp)
in its own team to translate \a sourcePath to plain text into \a destPath
with BTranslatorRoster::Translate() (which identifies the file itself),
completely isolated from this process. A translator crashing there costs
one lost analysis instead of taking index_server down with it (see #59,
#62, and the original haiku-os.org forum thread linked from the README).

A translator that merely hangs is handled by killing the helper team
outright once \a timeout passes - unlike the in-process RunWithTimeout.h
pattern this replaces for this one call site, that's safe here precisely
because the hung code never shared this process's own heap or locks to
begin with.

Returns B_OK on success (\a destPath now holds the translated text),
B_TIMED_OUT if the helper had to be killed, or another error for anything
else (helper binary missing, no translator claims the file, a crash). */
status_t	run_translator_helper(const char* sourcePath,
				const char* destPath, bigtime_t timeout);


#endif // RUN_TRANSLATOR_HELPER_H
