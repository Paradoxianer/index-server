/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "RunTranslatorHelper.h"

#include "IndexServerPrivate.h"
#include "RunIsolatedHelper.h"


status_t
run_translator_helper(const char* sourcePath, const char* destPath,
	bigtime_t timeout)
{
	const char* argv[] = { kTranslateHelperPath.String(), sourcePath,
		destPath, NULL };
	return run_isolated_helper(argv, timeout);
}
