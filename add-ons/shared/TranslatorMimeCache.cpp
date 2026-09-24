/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "TranslatorMimeCache.h"

#include <map>
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


// Serializes access to sCachesByOutputType (below) and to
// BTranslatorRoster::Default() while building it - concurrent access to
// that one process-wide roster from two volumes' worker threads at once
// has been observed corrupting its internal state badly enough to crash
// later in unrelated code (see FullTextAnalyser's history, #59/#62).
BLocker sTranslatorLock("translator mime cache lock");

// One cache per requested output type, built lazily the first time each
// is actually asked for - FullTextAnalyser only ever needs the
// B_TRANSLATOR_TEXT one, ThumbnailAnalyser only B_TRANSLATOR_BITMAP, so
// building the other on demand rather than always building both avoids
// wasted work for whichever add-on doesn't need it. Cleared entirely (not
// just one entry) by TranslatorWatcher below whenever the roster changes,
// so the next call for whichever output type rebuilds from the
// now-current roster - installing or removing a translator while
// index_server is already running takes effect on the next file
// analysed, not only after a restart.
std::map<uint32, std::set<BString, CaseInsensitiveLess>*> sCachesByOutputType;


// Listens for BTranslatorRoster::StartWatching()'s B_TRANSLATOR_ADDED/
// B_TRANSLATOR_REMOVED notifications and invalidates sCachesByOutputType
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
			for (std::map<uint32, std::set<BString, CaseInsensitiveLess>*>
					::iterator it = sCachesByOutputType.begin();
					it != sCachesByOutputType.end(); ++it) {
				delete it->second;
			}
			sCachesByOutputType.clear();
		} else
			BHandler::MessageReceived(message);
	}
};

TranslatorWatcher sTranslatorWatcher;
bool sTranslatorWatcherRegistered = false;


}	// namespace


bool
translator_supports_mime_type(const char* mimeType, uint32 outputType)
{
	BAutolock lock(sTranslatorLock);

	// Registered here, lazily, the same first time a cache below is built
	// - not in some separate one-time startup path, so a translator
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

	std::set<BString, CaseInsensitiveLess>*& cache
		= sCachesByOutputType[outputType];
	if (cache == NULL) {
		cache = new std::set<BString, CaseInsensitiveLess>;

		translator_id* ids;
		int32 count;
		if (BTranslatorRoster::Default()->GetAllTranslators(&ids, &count)
				== B_OK) {
			for (int32 i = 0; i < count; i++) {
				// A translator declaring an input MIME type says nothing
				// about what it can actually produce from it - PDFText
				// Translator, for instance, declares "application/pdf" as
				// input but only ever produces B_TRANSLATOR_TEXT, never
				// B_TRANSLATOR_BITMAP. Only translators whose own
				// GetOutputFormats() declares the requested type count -
				// checked per translator, not globally, precisely because
				// declaring an input format is not a promise about every
				// output format.
				const translation_format* outputFormats;
				int32 numOutputFormats;
				if (BTranslatorRoster::Default()->GetOutputFormats(ids[i],
						&outputFormats, &numOutputFormats) != B_OK) {
					continue;
				}
				bool producesRequestedType = false;
				for (int32 k = 0; k < numOutputFormats; k++) {
					if (outputFormats[k].type == outputType) {
						producesRequestedType = true;
						break;
					}
				}
				if (!producesRequestedType)
					continue;

				const translation_format* inputFormats;
				int32 numInputFormats;
				if (BTranslatorRoster::Default()->GetInputFormats(ids[i],
						&inputFormats, &numInputFormats) != B_OK) {
					continue;
				}
				for (int32 j = 0; j < numInputFormats; j++) {
					if (inputFormats[j].MIME[0] != '\0')
						cache->insert(inputFormats[j].MIME);
				}
			}
			delete[] ids;
		}
	}

	return cache->find(mimeType) != cache->end();
}
