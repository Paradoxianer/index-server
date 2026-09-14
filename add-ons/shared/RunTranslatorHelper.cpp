/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "RunTranslatorHelper.h"

#include <image.h>
#include <unistd.h>

#include "IndexServerPrivate.h"


extern char** environ;


status_t
run_translator_helper(const char* sourcePath, const char* destPath,
	bigtime_t timeout)
{
	const char* argv[5];
	int argc = 0;
	argv[argc++] = kTranslateHelperPath.String();
	if (destPath != NULL) {
		argv[argc++] = "translate";
		argv[argc++] = sourcePath;
		argv[argc++] = destPath;
	} else {
		argv[argc++] = "identify";
		argv[argc++] = sourcePath;
	}
	argv[argc] = NULL;

	thread_id thread = load_image(argc, argv, (const char**)environ);
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
		// Safe to just kill the whole team outright, unlike the in-process
		// RunWithTimeout.h pattern used elsewhere in this project - the
		// hung code here never held any lock or heap state of ours to
		// begin with.
		kill_team(info.team);
		return B_TIMED_OUT;
	}
	if (status != B_OK)
		return status;

	return exitValue == 0 ? B_OK : B_ERROR;
}
