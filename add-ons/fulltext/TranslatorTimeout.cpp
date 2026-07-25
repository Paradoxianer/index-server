/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "TranslatorTimeout.h"

#include <new>


namespace {


struct call_params {
	translator_call		call;
	void*				cookie;
	translator_cleanup	cleanup;
	sem_id				doneSemaphore;
	status_t			result;
	// 0 while neither side has finished yet; whichever side gets back the
	// pre-increment value of 0 arrived first and leaves params for the
	// other side, whichever gets back 1 arrived second and knows the other
	// side already did its part.
	int32				finishOrder;
};


status_t
run_call(void* data)
{
	call_params* params = (call_params*)data;
	params->result = params->call(params->cookie);

	if (atomic_add(&params->finishOrder, 1) == 0) {
		// The caller may still be waiting; let it take it from here.
		release_sem(params->doneSemaphore);
	} else {
		// The caller already timed out and moved on without us; it is our
		// job now to clean up everything, including params itself.
		if (params->cleanup != NULL)
			params->cleanup(params->cookie);
		delete_sem(params->doneSemaphore);
		delete params;
	}
	return B_OK;
}


}	// namespace


status_t
run_with_timeout(translator_call call, void* cookie,
	translator_cleanup cleanup, bigtime_t timeout)
{
	call_params* params = new(std::nothrow) call_params;
	if (params == NULL)
		return B_NO_MEMORY;
	params->call = call;
	params->cookie = cookie;
	params->cleanup = cleanup;
	params->result = B_ERROR;
	params->finishOrder = 0;

	params->doneSemaphore = create_sem(0, "translator call done");
	if (params->doneSemaphore < 0) {
		status_t error = params->doneSemaphore;
		delete params;
		return error;
	}

	thread_id thread = spawn_thread(run_call, "translator call",
		B_LOW_PRIORITY, params);
	if (thread < 0) {
		status_t error = thread;
		delete_sem(params->doneSemaphore);
		delete params;
		return error;
	}
	resume_thread(thread);

	status_t status = acquire_sem_etc(params->doneSemaphore, 1,
		B_RELATIVE_TIMEOUT, timeout);
	if (status == B_OK) {
		status = params->result;
		delete_sem(params->doneSemaphore);
		delete params;
		return status;
	}

	// We timed out. Find out, atomically, whether the helper thread had
	// already finished (and is about to release, or already released, the
	// semaphore right as we gave up).
	if (atomic_add(&params->finishOrder, 1) != 0) {
		// It finished first after all; the result is genuinely available.
		acquire_sem(params->doneSemaphore);
		status = params->result;
		delete_sem(params->doneSemaphore);
		delete params;
		return status;
	}

	// It is still running (or truly hung). Ownership of params and cookie
	// passes to it; it will clean up both whenever run_call() returns.
	return B_TIMED_OUT;
}
