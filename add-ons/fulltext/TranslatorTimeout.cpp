/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "TranslatorTimeout.h"


namespace {


struct call_params {
	translator_call	call;
	void*			cookie;
	sem_id			doneSemaphore;
	status_t		result;
};


status_t
run_call(void* data)
{
	call_params* params = (call_params*)data;
	params->result = params->call(params->cookie);
	release_sem(params->doneSemaphore);
	return B_OK;
}


}	// namespace


status_t
run_with_timeout(translator_call call, void* cookie, bigtime_t timeout)
{
	call_params params;
	params.call = call;
	params.cookie = cookie;
	params.result = B_ERROR;

	params.doneSemaphore = create_sem(0, "translator call done");
	if (params.doneSemaphore < 0)
		return params.doneSemaphore;

	thread_id thread = spawn_thread(run_call, "translator call",
		B_LOW_PRIORITY, &params);
	if (thread < 0) {
		delete_sem(params.doneSemaphore);
		return thread;
	}
	resume_thread(thread);

	status_t status = acquire_sem_etc(params.doneSemaphore, 1,
		B_RELATIVE_TIMEOUT, timeout);
	if (status == B_TIMED_OUT)
		kill_thread(thread);
	delete_sem(params.doneSemaphore);

	return status == B_OK ? params.result : status;
}
