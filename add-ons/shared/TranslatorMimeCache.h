/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef TRANSLATOR_MIME_CACHE_H
#define TRANSLATOR_MIME_CACHE_H


#include <SupportDefs.h>


/*! Whether some installed translator both declares \a mimeType as a format
it can read (per GetInputFormats()) and declares \a outputType among what
it can produce (per GetOutputFormats()) - checked for the same translator,
not "some translator reads this" and "some translator writes that"
independently of each other. A translator declaring an input MIME type is
not a promise about every output format it can produce from it: PDFText
Translator, for instance, declares "application/pdf" as input but only
ever produces B_TRANSLATOR_TEXT, never B_TRANSLATOR_BITMAP.

This is still only ever a coarse pre-filter, not a guarantee the specific
call will succeed - BTranslatorRoster::Identify() itself asks every
registered translator regardless of hint (the mimeType passed to
Identify() is purely advisory), and at least one installed translator has
been found not to honor its own declared input formats when deciding what
to claim (see FullTextAnalyser's history, issue #27). So this can't
replace an actual Identify()/Translate() call - it exists purely to avoid
making that call at all for a (format, desired output) combination nothing
installed even claims to support, which matters both for performance (no
point probing every translator for nothing) and safety (issue #47: a
translator handed content of a completely unrelated format has been found
to corrupt heap memory rather than just declining cleanly).

Cached lazily, one cache per distinct \a outputType actually asked for,
per calling add-on image. BTranslatorRoster::Default() is one process-wide
roster, but each add-on here is a separate loaded image with its own
static storage, so this cache and the lock guarding it end up duplicated
per add-on rather than truly shared across them - harmless, just not
deduplicated. Invalidated automatically via BTranslatorRoster::
StartWatching() whenever a translator is installed or removed while
index_server is already running, so that takes effect on the next file
analysed rather than only after a restart. */
bool	translator_supports_mime_type(const char* mimeType,
			uint32 outputType);


#endif // TRANSLATOR_MIME_CACHE_H
