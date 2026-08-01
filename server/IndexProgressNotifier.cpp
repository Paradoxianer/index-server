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
#include <Message.h>
#include <Notification.h>

#include "IndexServerPrivate.h"
#include "IndexServerSettings.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "IndexProgressNotifier"


// Update at most once a second - once per file would make the notification
// itself a source of load during a large catch up.
static const bigtime_t kMinNotifyInterval = 1000000;
// A routine catch up after a short restart is over in a blink; only a
// backlog large enough to actually take a while is worth popping up an OS
// notification for (see issue #34). Observers (e.g. the Settings window)
// still get every update regardless - a live progress display has no
// "is this worth interrupting the user for" concern the way a popup does.
static const int32 kNotifyThreshold = 200;
static const char* const kNotificationGroup = "Index Server";
// Clicking the notification opens the settings preflet; it has no status
// view yet (see issue #34's secondary part), but this is forward compatible
// with adding one later.
static const char* const kSettingsAppSignature =
	"application/x-vnd.Haiku-IndexServerSettings";


IndexProgressNotifier::IndexProgressNotifier(const BString& messageID,
	const BString& title, const BString& volumeName,
	IndexServerSettings* settings)
	:
	fMessageID(messageID),
	fTitle(title),
	fVolumeName(volumeName),
	fSettings(settings),
	fLastSent(0)
{
}


void
IndexProgressNotifier::Progress(int32 current, int32 total,
	const BString& currentPath)
{
	if (total <= 0)
		return;

	bool isFirst = (current == 0);
	bool isLast = (current >= total);
	bigtime_t now = system_time();
	if (!isFirst && !isLast && now - fLastSent < kMinNotifyInterval)
		return;
	fLastSent = now;

	_NotifyObservers(current, total, currentPath);

	if (total < kNotifyThreshold)
		return;

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
	_NotifyObservers(count, count, BString());

	if (count < kNotifyThreshold)
		return;

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


void
IndexProgressNotifier::_NotifyObservers(int32 current, int32 total,
	const BString& currentPath)
{
	if (fSettings == NULL)
		return;

	BMessage progress(kMsgIndexProgress);
	progress.AddInt32("current", current);
	progress.AddInt32("total", total);
	progress.AddString("volume", fVolumeName);
	progress.AddString("path", currentPath);
	fSettings->NotifyProgressObservers(progress);
}
