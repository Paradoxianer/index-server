/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */

// A small CLI client for index_server's query monitor protocol
// (kMsgStartQueryMonitor/kMsgQueryMonitorUpdate, IndexServerPrivate.h -
// see issue #19). Unlike QueryClient.cpp (a one-shot query), this one
// needs to actually receive pushed messages, so it runs a real
// BApplication message loop rather than staying loop-free.
//
// query_monitor_client <query> <durationSeconds> [maxResults] - registers
// a monitor for <query>, runs for <durationSeconds> printing one line per
// update to stdout ("added\tscore\tpath" or "removed\tpath"), then
// unregisters and exits. Used by tests/run_tests.sh; works standalone too.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <Entry.h>
#include <Message.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <Path.h>

#include "IndexServerPrivate.h"


static const uint32 kMsgStopRunning = 'QMCq';


class QueryMonitorApp : public BApplication {
public:
	QueryMonitorApp()
		:
		BApplication("application/x-vnd.Haiku-index_server_query_monitor_client"),
		fMonitorToken(-1)
	{
	}

	void SetMonitorToken(int32 token)
	{
		fMonitorToken = token;
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
		case kMsgQueryMonitorUpdate:
		{
			entry_ref ref;
			float score;
			for (int32 i = 0; message->FindRef("addedRefs", i, &ref) == B_OK;
					i++) {
				message->FindFloat("addedScores", i, &score);
				BPath path(&ref);
				printf("added\t%f\t%s\n", score, path.Path());
			}
			for (int32 i = 0; message->FindRef("removedRefs", i, &ref)
					== B_OK; i++) {
				BPath path(&ref);
				printf("removed\t%s\n", path.Path());
			}
			fflush(stdout);
			break;
		}

		case kMsgStopRunning:
		{
			if (fMonitorToken >= 0) {
				BMessage stop(kMsgStopQueryMonitor);
				stop.AddInt32("monitorToken", fMonitorToken);
				BMessenger(kIndexServerSignature.String()).SendMessage(
					&stop);
			}
			PostMessage(B_QUIT_REQUESTED);
			break;
		}

		default:
			BApplication::MessageReceived(message);
		}
	}

private:
	int32	fMonitorToken;
};


int
main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr,
			"usage: %s <query> <durationSeconds> [maxResults]\n", argv[0]);
		return 2;
	}

	const char* query = argv[1];
	bigtime_t duration = atoi(argv[2]) * 1000000LL;
	int32 maxResults = argc > 3 ? atoi(argv[3]) : 100;

	QueryMonitorApp app;

	BMessenger indexServer(kIndexServerSignature.String());
	if (!indexServer.IsValid()) {
		fprintf(stderr, "index_server is not running\n");
		return 2;
	}

	BMessage request(kMsgStartQueryMonitor);
	request.AddString("query", query);
	request.AddInt32("maxResults", maxResults);
	request.AddMessenger("target", BMessenger(&app));

	BMessage reply;
	status_t status = indexServer.SendMessage(&request, &reply, 15000000,
		15000000);
	if (status != B_OK) {
		fprintf(stderr, "kMsgStartQueryMonitor failed: %s\n",
			strerror(status));
		return 2;
	}

	int32 token;
	if (reply.FindInt32("monitorToken", &token) != B_OK) {
		fprintf(stderr, "kMsgStartQueryMonitor reply had no monitorToken\n");
		return 2;
	}
	app.SetMonitorToken(token);
	fprintf(stderr, "monitoring, token %" B_PRId32 "\n", token);

	BMessage stopMessage(kMsgStopRunning);
	BMessageRunner runner(BMessenger(&app), &stopMessage, duration, 1);

	app.Run();
	return 0;
}
