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

#include <Entry.h>
#include <Message.h>
#include <Messenger.h>
#include <Path.h>

#include "IndexServerPrivate.h"


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <query> [maxResults]\n", argv[0]);
		return 2;
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
