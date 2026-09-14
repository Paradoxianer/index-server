/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef RUN_ISOLATED_HELPER_H
#define RUN_ISOLATED_HELPER_H


#include <OS.h>


/*! Runs the executable named by \a argv[0] (a NULL-terminated argv array,
exec-style - the same convention load_image() itself takes) to completion
in its own team, isolated from this process. See RunTranslatorHelper.h and
RunThumbnailHelper.h, the two callers this exists for, for what problem
that solves and why.

A hang is handled by killing the helper's team outright once \a timeout
passes - safe here, unlike for an in-process hang, because code running in
a separate team never shared this process's own heap or locks to begin
with.

Returns B_OK if the helper exited with status 0, B_TIMED_OUT if it had to
be killed, or another error for anything else (helper missing, exited with
a nonzero status, a crash - all indistinguishable from here on purpose;
the caller only needs to know whether the result at its own output path,
if any, is trustworthy). */
status_t	run_isolated_helper(const char* const argv[], bigtime_t timeout);


#endif // RUN_ISOLATED_HELPER_H
