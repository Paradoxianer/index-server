/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */

// Standalone helper spawned by FullTextAnalyser (via RunTranslatorHelper.h,
// add-ons/shared/) to run exactly one BTranslatorRoster call - Identify()
// or Translate() - in a throwaway team of its own. BTranslatorRoster loads
// whatever third-party translator add-on claims a file's format and runs
// its code directly against untrusted file content; a translator that
// segfaults on malformed input used to take index_server itself down with
// it (concurrent-access corruption bugs #59 and #62 already forced
// FullTextAnalyser to serialize every call into the roster - see its own
// sTranslatorLock comment - but that never protected against a translator
// crashing outright on a single call). Running the call here instead means
// a crash costs one lost analysis, not the whole server: whatever happens
// to this team, RunTranslatorHelper.cpp's wait_for_thread_etc() in the
// parent returns either way.
//
// disable_debugger() below is why a crash here doesn't also pop up
// Haiku's own crash-report alert on the user's desktop - the debug_server
// would otherwise intercept the fault before this team's exit even
// reaches the parent's wait.
//
// usage:
//   IndexServerTranslateHelper identify <sourcePath>
//   IndexServerTranslateHelper translate <sourcePath> <destPath>
//
// Exit code 0 on success, nonzero on any handled failure (unsupported
// format, read/write error). A hard crash never returns an exit code at
// all - the parent tells that apart from "still running" purely by the
// team being gone, which is why "translate" only ever creates <destPath>
// itself on success: written to a sibling ".part" file first, then
// rename()d into place, so a crash mid-write can never leave a partial
// file sitting at the path the parent expects a finished result at.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <File.h>
#include <OS.h>
#include <String.h>
#include <TranslatorFormats.h>
#include <TranslatorRoster.h>


static int
do_identify(const char* sourcePath)
{
	BFile source(sourcePath, B_READ_ONLY);
	if (source.InitCheck() != B_OK)
		return 1;

	translator_info info;
	status_t status = BTranslatorRoster::Default()->Identify(&source, NULL,
		&info, 0, NULL, B_TRANSLATOR_TEXT);
	return status == B_OK ? 0 : 1;
}


static int
do_translate(const char* sourcePath, const char* destPath)
{
	BFile source(sourcePath, B_READ_ONLY);
	if (source.InitCheck() != B_OK)
		return 1;

	BString tempPath(destPath);
	tempPath << ".part";

	status_t status;
	{
		// Scoped so the BFile destructor flushes and closes destination
		// before the rename() below - Translate() writing through a
		// BPositionIO doesn't itself guarantee the data has left the file
		// cache until the BFile is done with it.
		BFile destination(tempPath.String(),
			B_READ_WRITE | B_CREATE_FILE | B_ERASE_FILE);
		if (destination.InitCheck() != B_OK)
			return 1;

		status = BTranslatorRoster::Default()->Translate(&source, NULL, NULL,
			&destination, 'TEXT');
	}

	if (status != B_OK) {
		remove(tempPath.String());
		return 1;
	}

	if (rename(tempPath.String(), destPath) != 0) {
		remove(tempPath.String());
		return 1;
	}
	return 0;
}


int
main(int argc, char** argv)
{
	// Never let a fault here trigger Haiku's own crash-report dialog - see
	// this file's own comment above for why.
	disable_debugger(1);

	if (argc == 3 && strcmp(argv[1], "identify") == 0)
		return do_identify(argv[2]);
	if (argc == 4 && strcmp(argv[1], "translate") == 0)
		return do_translate(argv[2], argv[3]);

	fprintf(stderr,
		"usage: %s identify <path> | translate <path> <destPath>\n",
		argv[0]);
	return 2;
}
