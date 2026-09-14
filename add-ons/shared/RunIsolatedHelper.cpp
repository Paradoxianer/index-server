/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "RunIsolatedHelper.h"

#include <image.h>


extern char** environ;


status_t
run_isolated_helper(const char* const argv[], bigtime_t timeout)
{
	int argc = 0;
	while (argv[argc] != NULL)
		argc++;

	thread_id thread = load_image(argc, (const char**)argv,
		(const char**)environ);
	if (thread < 0)
		return thread;

	// Fetch the team id before resuming - once resumed, the helper may run
	// to completion (and its thread id become invalid) before we'd get a
	// chance to look it up otherwise.
	thread_info info;
	status_t infoStatus = get_thread_info(thread, &info);
	if (infoStatus != B_OK) {
		kill_thread(thread);
		return infoStatus;
	}

	resume_thread(thread);

	status_t exitValue;
	status_t status = wait_for_thread_etc(thread, B_RELATIVE_TIMEOUT,
		timeout, &exitValue);
	if (status == B_TIMED_OUT) {
		kill_team(info.team);
		return B_TIMED_OUT;
	}
	if (status != B_OK)
		return status;

	return exitValue == 0 ? B_OK : B_ERROR;
}
