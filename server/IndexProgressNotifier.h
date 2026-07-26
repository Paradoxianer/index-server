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


// Reports the progress of a single catch up pass as a system notification
// (see issue #34). Not meant for per-file live monitoring - only for the
// initial full scan and any larger backlog after a restart, both of which
// can run long enough that silent disk activity looks like a hang.
class IndexProgressNotifier {
public:
								IndexProgressNotifier(const BString& messageID,
									const BString& title);

			void				Progress(int32 current, int32 total);
			void				Done(int32 count);

private:
			BString				fMessageID;
			BString				fTitle;
			bigtime_t			fLastSent;
};

#endif // INDEX_PROGRESS_NOTIFIER_H
