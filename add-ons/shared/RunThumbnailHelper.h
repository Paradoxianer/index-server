/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef RUN_THUMBNAIL_HELPER_H
#define RUN_THUMBNAIL_HELPER_H


#include <OS.h>


/*! Runs IndexServerThumbnailHelper (thumbnail-helper/ThumbnailHelper.cpp)
in its own team to decode \a sourcePath, compose a fixed-size thumbnail,
and encode it, completely isolated from this process - same reasoning as
RunTranslatorHelper.h: BTranslationUtils::GetBitmap() and
BTranslatorRoster::Translate() both run whatever third-party translator
add-on claims the image's format against untrusted content, and a crash
there must not take index_server down with it.

Unlike the translate helper, this one needs a live app_server connection
(BBitmap/BView composition hangs indefinitely without one - confirmed
empirically under BServer(initGUI=false)), so the helper is a plain
BApplication rather than headless; nothing about that changes how it's
run from here.

On success, \a destPath holds the encoded WebP thumbnail (via a sibling
".part" file renamed into place, same atomicity reasoning as
TranslateHelper.cpp - a crash mid-write can never leave a partial file at
the path this expects a finished one at). Returns B_OK on success,
B_TIMED_OUT if the helper had to be killed after \a timeout, or another
error for anything else (helper binary missing, the image didn't decode,
a crash). */
status_t	run_thumbnail_helper(const char* sourcePath, const char* destPath,
				bigtime_t timeout);


#endif // RUN_THUMBNAIL_HELPER_H
