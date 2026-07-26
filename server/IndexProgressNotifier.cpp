/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "IndexProgressNotifier.h"

#include <stdio.h>
#include <string.h>

#include <Catalog.h>
#include <Notification.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "IndexProgressNotifier"


// Update at most once a second - once per file would make the notification
// itself a source of load during a large catch up.
static const bigtime_t kMinNotifyInterval = 1000000;
static const char* const kNotificationGroup = "Index Server";
// Clicking the notification opens the settings preflet; it has no status
// view yet (see issue #34's secondary part), but this is forward compatible
// with adding one later.
static const char* const kSettingsAppSignature =
	"application/x-vnd.Haiku-IndexServerSettings";


IndexProgressNotifier::IndexProgressNotifier(const BString& messageID,
	const BString& title)
	:
	fMessageID(messageID),
	fTitle(title),
	fLastSent(0)
{
}


void
IndexProgressNotifier::Progress(int32 current, int32 total)
{
	if (total <= 0)
		return;

	bool isFirst = (current == 0);
	bool isLast = (current >= total);
	bigtime_t now = system_time();
	if (!isFirst && !isLast && now - fLastSent < kMinNotifyInterval)
		return;
	fLastSent = now;

	BNotification notification(B_PROGRESS_NOTIFICATION);
	notification.SetGroup(kNotificationGroup);
	notification.SetTitle(fTitle);
	notification.SetMessageID(fMessageID);
	notification.SetProgress((float)current / (float)total);
	notification.SetOnClickApp(kSettingsAppSignature);

	BString content;
	content.SetToFormat(B_TRANSLATE("%ld / %ld files"), (long)current,
		(long)total);
	notification.SetContent(content);

	// Refreshed well within the timeout by the next progress update, or
	// replaced by Done()'s notification once catch up finishes; the timeout
	// just keeps a stale bar from lingering if the server dies mid catch up.
	status_t status = notification.Send(10000000);
	if (status != B_OK)
		printf("IndexProgressNotifier: Send() failed: %s\n", strerror(status));
}


void
IndexProgressNotifier::Done(int32 count)
{
	BNotification notification(B_INFORMATION_NOTIFICATION);
	notification.SetGroup(kNotificationGroup);
	notification.SetTitle(fTitle);
	notification.SetMessageID(fMessageID);
	notification.SetOnClickApp(kSettingsAppSignature);

	BString content;
	content.SetToFormat(B_TRANSLATE("%ld files indexed"), (long)count);
	notification.SetContent(content);

	status_t status = notification.Send(5000000);
	if (status != B_OK)
		printf("IndexProgressNotifier: Send() failed: %s\n", strerror(status));
}
