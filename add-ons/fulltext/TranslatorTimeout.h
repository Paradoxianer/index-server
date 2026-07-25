/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef TRANSLATOR_TIMEOUT_H
#define TRANSLATOR_TIMEOUT_H


#include <OS.h>


typedef status_t (*translator_call)(void* cookie);
typedef void (*translator_cleanup)(void* cookie);


/*! Runs \a call on a helper thread and gives up after \a timeout rather
than risking an indefinite hang on a misbehaving translator.

On B_TIMED_OUT, \a cookie is *not* cleaned up here: the helper thread may
still be using it (still inside \a call, possibly forever), so ownership
of \a cookie passes to that thread, which will invoke \a cleanup on it
once \a call eventually returns. This deliberately never kills the helper
thread - killing a thread that happens to be holding e.g. the malloc heap
lock corrupts that state for the whole team, which is worse than a leaked
thread and allocation on the rare truly-hung translator.

On any other return value, the caller keeps ownership of \a cookie and
must clean it up itself (\a cleanup is not called for it). */
status_t	run_with_timeout(translator_call call, void* cookie,
				translator_cleanup cleanup, bigtime_t timeout);


#endif // TRANSLATOR_TIMEOUT_H
