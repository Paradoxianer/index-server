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


//! Runs \a call on a helper thread and gives up after \a timeout, killing
//! the thread rather than risking an indefinite hang on a misbehaving
//! translator. \a call must only touch \a cookie and must not be called
//! again (e.g. retried) while a previous call may still be running after a
//! timeout.
status_t	run_with_timeout(translator_call call, void* cookie,
				bigtime_t timeout);


#endif // TRANSLATOR_TIMEOUT_H
