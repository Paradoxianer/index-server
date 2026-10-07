/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */

// A small CLI client that talks directly to index_server's own query
// protocol (kMsgQuery/kMsgQueryReply, IndexServerPrivate.h) - not a GUI,
// so it doesn't depend on IndexServerSearch or Haiku's own `hey` scripting
// (whose window/menu addressing has repeatedly proven unreliable for
// automated testing). Used by tests/run_tests.sh, but works standalone
// too: `query_client "some query"` prints one "score\tpath" line per
// result to stdout, a one-line summary to stderr.
//
// A synchronous two-way BMessenger::SendMessage() is fine here, unlike in
// SearchWindow (see its own comment on why it deliberately avoids this) -
// there's no GUI message loop here for a slow query to block.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Entry.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <Path.h>

#include "IndexServerPrivate.h"


// Polls kMsgGetStatus until index_server reports no catch up or live
// analysis in progress on any volume, or until timeoutSeconds elapses.
// Added for tests/run_tests.sh: a volume with a large backlog (e.g. a
// full source tree freshly attached to the test VM) can hold the
// CLucene write lock for long enough that the suite's fixed post-start
// sleep isn't enough, and every regression_test() run restarts the
// server from scratch - paying that same catch up cost again every
// time - so a fixed sleep either wastes time once the backlog is gone
// or isn't enough while it's still there. Returns 0 once idle, 1 on
// timeout.
static int
wait_idle(bigtime_t timeoutSeconds)
{
	BMessenger messenger(kIndexServerSignature.String());
	if (!messenger.IsValid()) {
		fprintf(stderr, "index_server is not running\n");
		return 2;
	}

	bigtime_t deadline = system_time() + timeoutSeconds * 1000000;
	while (system_time() < deadline) {
		BMessage request(kMsgGetStatus);
		BMessage reply;
		status_t status = messenger.SendMessage(&request, &reply, 5000000,
			5000000);
		bool indexing = true;
		if (status == B_OK)
			reply.FindBool("indexing", &indexing);
		if (!indexing) {
			fprintf(stderr, "index_server is idle\n");
			return 0;
		}
		snooze(1000000);
	}
	fprintf(stderr, "index_server still indexing after %" B_PRId64
		" second(s)\n", timeoutSeconds);
	return 1;
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <query> [maxResults]\n", argv[0]);
		fprintf(stderr, "       %s --wait-idle [timeoutSeconds]\n", argv[0]);
		return 2;
	}

	if (strcmp(argv[1], "--wait-idle") == 0) {
		bigtime_t timeoutSeconds = argc > 2 ? atoi(argv[2]) : 300;
		return wait_idle(timeoutSeconds);
	}

	BMessage request(kMsgQuery);
	request.AddString("query", argv[1]);
	if (argc > 2)
		request.AddInt32("maxResults", atoi(argv[2]));

	BMessenger messenger(kIndexServerSignature.String());
	if (!messenger.IsValid()) {
		fprintf(stderr, "index_server is not running\n");
		return 2;
	}

	BMessage reply;
	status_t status = messenger.SendMessage(&request, &reply, 15000000,
		15000000);
	if (status != B_OK) {
		fprintf(stderr, "query failed: %s\n", strerror(status));
		return 2;
	}

	entry_ref ref;
	int32 count = 0;
	for (int32 i = 0; reply.FindRef("refs", i, &ref) == B_OK; i++) {
		float score = 0;
		reply.FindFloat("scores", i, &score);
		BPath path(&ref);
		printf("%.3f\t%s\n", score, path.Path());
		count++;
	}

	int32 searchedVolumes = 0;
	reply.FindInt32("searchedVolumes", &searchedVolumes);
	fprintf(stderr, "%" B_PRId32 " result(s), %" B_PRId32 " volume(s) "
		"searched\n", count, searchedVolumes);
	return 0;
}
