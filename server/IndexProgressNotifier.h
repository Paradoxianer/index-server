/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef INDEX_PROGRESS_NOTIFIER_H
#define INDEX_PROGRESS_NOTIFIER_H


#include <String.h>
#include <SupportDefs.h>


class IndexServerSettings;


// Reports the progress of a single catch up pass both as a system
// notification (see issue #34) and, live, to any BMessenger registered via
// kMsgRegisterProgressObserver (e.g. the Settings preflet - see issue #43's
// follow-up discussion). Not meant for per-file live monitoring - only for
// the initial full scan and any larger backlog after a restart, both of
// which can run long enough that silent disk activity looks like a hang.
class IndexProgressNotifier {
public:
								IndexProgressNotifier(const BString& messageID,
									const BString& title,
									const BString& volumeName,
									IndexServerSettings* settings);

			//! currentPath may be empty if not known/relevant for this
			//! update.
			void				Progress(int32 current, int32 total,
									const BString& currentPath = BString());
			void				Done(int32 count);

private:
			void				_NotifyObservers(int32 current, int32 total,
									const BString& currentPath);

			BString				fMessageID;
			BString				fTitle;
			BString				fVolumeName;
			IndexServerSettings*	fSettings;
			bigtime_t			fLastSent;
};

#endif // INDEX_PROGRESS_NOTIFIER_H
