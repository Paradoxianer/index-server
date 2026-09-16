/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "TranslatorMimeCache.h"

#include <set>

#include <Application.h>
#include <Autolock.h>
#include <Handler.h>
#include <Locker.h>
#include <Messenger.h>
#include <String.h>
#include <TranslatorRoster.h>


namespace {


// MIME types compare case-insensitively per BMimeType's own documented
// equality rule (see #49).
struct CaseInsensitiveLess {
	bool operator()(const BString& a, const BString& b) const
	{
		return a.ICompare(b) < 0;
	}
};


// Serializes access to sSupportedMimeTypes (below) and to
// BTranslatorRoster::Default() while building it - concurrent access to
// that one process-wide roster from two volumes' worker threads at once
// has been observed corrupting its internal state badly enough to crash
// later in unrelated code (see FullTextAnalyser's history, #59/#62).
BLocker sTranslatorLock("translator mime cache lock");

// Built once, lazily, the first time it's needed rather than eagerly at
// startup, so this never adds its own delay to an analyser's constructor
// (which typically runs synchronously for every mounted volume at once
// from IndexServer::ReadyToRun()). Set back to NULL (not just cleared) by
// TranslatorWatcher below whenever the roster changes, so the next call
// rebuilds it the same lazy way.
std::set<BString, CaseInsensitiveLess>* sSupportedMimeTypes = NULL;


// Listens for BTranslatorRoster::StartWatching()'s B_TRANSLATOR_ADDED/
// B_TRANSLATOR_REMOVED notifications and invalidates sSupportedMimeTypes
// so it gets rebuilt from the now-current roster. Needs a real BLooper to
// receive messages on, which this shared code doesn't have one of its
// own - attached to be_app (the calling add-on's own team's
// BApplication, valid from any loaded add-on in that team) instead.
class TranslatorWatcher : public BHandler {
public:
	TranslatorWatcher()
		:
		BHandler("translator mime cache watcher")
	{
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == B_TRANSLATOR_ADDED
			|| message->what == B_TRANSLATOR_REMOVED) {
			BAutolock lock(sTranslatorLock);
			delete sSupportedMimeTypes;
			sSupportedMimeTypes = NULL;
		} else
			BHandler::MessageReceived(message);
	}
};

TranslatorWatcher sTranslatorWatcher;
bool sTranslatorWatcherRegistered = false;


}	// namespace


bool
translator_supports_mime_type(const char* mimeType)
{
	BAutolock lock(sTranslatorLock);

	// Registered here, lazily, the same first time the cache below is
	// built - not in some separate one-time startup path, so a translator
	// installed later still gets watched from that point on regardless of
	// when the calling add-on's own first real call happens to land.
	// BLooper::AddHandler() requires its target locked, and this runs on a
	// VolumeWorker thread, not be_app's own.
	if (!sTranslatorWatcherRegistered && be_app != NULL && be_app->Lock()) {
		be_app->AddHandler(&sTranslatorWatcher);
		be_app->Unlock();
		BTranslatorRoster::Default()->StartWatching(
			BMessenger(&sTranslatorWatcher));
		sTranslatorWatcherRegistered = true;
	}

	if (sSupportedMimeTypes == NULL) {
		sSupportedMimeTypes = new std::set<BString, CaseInsensitiveLess>;

		translator_id* ids;
		int32 count;
		if (BTranslatorRoster::Default()->GetAllTranslators(&ids, &count)
				== B_OK) {
			for (int32 i = 0; i < count; i++) {
				const translation_format* formats;
				int32 numFormats;
				if (BTranslatorRoster::Default()->GetInputFormats(ids[i],
						&formats, &numFormats) != B_OK) {
					continue;
				}
				for (int32 j = 0; j < numFormats; j++) {
					if (formats[j].MIME[0] != '\0')
						sSupportedMimeTypes->insert(formats[j].MIME);
				}
			}
			delete[] ids;
		}
	}

	return sSupportedMimeTypes->find(mimeType) != sSupportedMimeTypes->end();
}
