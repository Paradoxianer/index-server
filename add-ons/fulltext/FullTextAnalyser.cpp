/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */
#include "FullTextAnalyser.h"

#include <new>
#include <string.h>
#include <strings.h>

#include <Autolock.h>
#include <File.h>
#include <Locker.h>
#include <Node.h>
#include <NodeInfo.h>
#include <String.h>
#include <TranslatorFormats.h>
#include <TranslatorRoster.h>

#include "CLuceneDataBase.h"
#include "IndexServerPrivate.h"
#include "RunWithTimeout.h"


#define DEBUG_FULLTEXT_ANALYSER
#ifdef DEBUG_FULLTEXT_ANALYSER
#include <stdio.h>
#	define STRACE(x...) printf("FullTextAnalyser: " x)
#else
#	define STRACE(x...) ;
#endif


// Identify() is normally just header sniffing, but a misbehaving translator
// must not be allowed to stall the whole VolumeWorker thread over it.
const bigtime_t kIdentifyTimeout = 5 * 1000000;

// A hung or pathological translator must not stall the whole VolumeWorker
// thread either (it processes every entry of a volume serially).
const bigtime_t kTranslateTimeout = 30 * 1000000;

// See _ReportSlowEntry()'s comment. Deliberately well under a second - a
// stretch of many small files each individually taking a few hundred ms
// (never crossing a 1s bar on their own) still adds up to the same
// notification-refresh gap as one genuinely slow file.
const bigtime_t kSlowEntryThreshold = 200 * 1000;

// Each volume gets its own FullTextAnalyser instance running on its own
// VolumeWorker thread, but BTranslatorRoster::Default() is one process-wide
// roster - concurrent Identify()/Translate() calls from two volumes at once
// have been observed corrupting a translator's own internal state badly
// enough to crash later in unrelated code (see #59, #62). Serialize every
// call into it.
static BLocker sTranslatorLock("translator lock");


namespace {


// Owns the file itself rather than pointing at the caller's stack local: on
// timeout, ownership passes to the still-running helper thread (see
// RunWithTimeout.h), which may still be reading it.
struct identify_cookie {
	BFile*				source;
	translator_info		info;
};


status_t
do_identify(void* data)
{
	identify_cookie* cookie = (identify_cookie*)data;
	BAutolock lock(sTranslatorLock);
	return BTranslatorRoster::Default()->Identify(cookie->source, NULL,
		&cookie->info, 0, NULL, B_TRANSLATOR_TEXT);
}


void
cleanup_identify(void* data)
{
	identify_cookie* cookie = (identify_cookie*)data;
	delete cookie->source;
	delete cookie;
}


// Owns the files itself rather than pointing at the caller's stack locals:
// on timeout, ownership passes to the still-running helper thread (see
// RunWithTimeout.h), which may still be reading/writing them.
struct translate_cookie {
	BFile*	source;
	BFile*	destination;
};


status_t
do_translate(void* data)
{
	translate_cookie* cookie = (translate_cookie*)data;
	bigtime_t lockWaitStart = system_time();
	BAutolock lock(sTranslatorLock);
	bigtime_t translateStart = system_time();
	status_t status = BTranslatorRoster::Default()->Translate(cookie->source,
		NULL, NULL, cookie->destination, 'TEXT');
	bigtime_t elapsed = system_time() - translateStart;
	if (elapsed > kSlowEntryThreshold) {
		STRACE("slow Translate() (%" B_PRId64 " ms, waited %" B_PRId64
			" ms for sTranslatorLock)\n", elapsed / 1000,
			(translateStart - lockWaitStart) / 1000);
	}
	return status;
}


void
cleanup_translate(void* data)
{
	translate_cookie* cookie = (translate_cookie*)data;
	delete cookie->source;
	delete cookie->destination;
	delete cookie;
}


}	// namespace


FullTextAnalyser::FullTextAnalyser(BString name, const BVolume& volume)
	:
	FileAnalyser(name, volume),

	fWriteDataBase(NULL),
	fNUncommited(0)
{
	fDataBasePath = volume_index_server_directory(volume);
	status_t status = fDataBasePath.Append(kFullTextDirectory);

	if (status == B_OK)
		fWriteDataBase = new CLuceneWriteDataBase(fDataBasePath);
}


FullTextAnalyser::~FullTextAnalyser()
{
	_DeletePendingTempFiles();
	delete fWriteDataBase;
}


status_t
FullTextAnalyser::InitCheck()
{
	if (fDataBasePath.InitCheck() != B_OK)
		return fDataBasePath.InitCheck();
	if (!fWriteDataBase)
		return B_NO_MEMORY;

	return fWriteDataBase->InitCheck();
}


void
FullTextAnalyser::AnalyseEntry(const entry_ref& ref)
{
	bigtime_t start = system_time();

	if (!_InterestingEntry(ref)) {
		_ReportSlowEntry(ref, start, "not interesting");
		return;
	}

	//STRACE("FullTextAnalyser AnalyseEntry: %s %s\n", ref.name, path.Path());
	if (_IsPlainText(ref)) {
		fWriteDataBase->AddDocument(ref);
	} else if (!_QueueTranslated(ref)) {
		_ReportSlowEntry(ref, start, "translate failed");
		return;
	}

	_ReportSlowEntry(ref, start, "indexed");

	fNUncommited++;
	if (fNUncommited > 100)
		LastEntry();
}


// A slow individual entry - waiting on kTranslateTimeout, kIdentifyTimeout,
// sTranslatorLock, or the shared CLucene write lock, all of which can each
// individually take several seconds under load - delays every Progress()
// call after it by however long it took, since AnalyseEntry() is called
// synchronously once per entry from CatchUpAnalyser::_CatchUp()'s loop. If
// that gap outlasts the progress notification's own refresh window, the
// notification just disappears until the next entry's update arrives -
// looks like it randomly vanishing, but is really this. Logging only the
// slow ones (instead of timing every entry unconditionally) keeps this from
// flooding the log during a large, otherwise unremarkable catch up.
void
FullTextAnalyser::_ReportSlowEntry(const entry_ref& ref, bigtime_t start,
	const char* outcome)
{
	bigtime_t elapsed = system_time() - start;
	if (elapsed > kSlowEntryThreshold) {
		STRACE("slow entry (%" B_PRId64 " ms, %s): %s\n",
			elapsed / 1000, outcome, ref.name);
	}
}


void
FullTextAnalyser::DeleteEntry(const entry_ref& ref)
{
	if (_IsInIndexDirectory(ref))
		return;
	STRACE("FullTextAnalyser DeleteEntry: %s\n", ref.name);
	fWriteDataBase->RemoveDocument(ref);
}


void
FullTextAnalyser::MoveEntry(const entry_ref& oldRef, const entry_ref& newRef)
{
	if (!_InterestingEntry(newRef))
		return;
	STRACE("FullTextAnalyser MoveEntry: %s to %s\n", oldRef.name, newRef.name);
	fWriteDataBase->RemoveDocument(oldRef);
	AnalyseEntry(newRef);
}


void
FullTextAnalyser::LastEntry()
{
	fWriteDataBase->Commit();
	_DeletePendingTempFiles();
	fNUncommited = 0;
}


status_t
FullTextAnalyser::HandleQuery(const BMessage& query, BMessage& reply)
{
	BString queryString;
	if (query.FindString("query", &queryString) != B_OK)
		return B_BAD_VALUE;

	// BMessage::FindInt32() zeroes *value up front even when the field is
	// missing (see BMessage.cpp's DEFINE_FUNCTIONS macro), so the usual
	// "declare with a default, ignore a failed Find" idiom silently
	// clobbers the default to 0 - check the status explicitly instead.
	int32 maxResults = 100;
	int32 requestedMax;
	if (query.FindInt32("maxResults", &requestedMax) == B_OK)
		maxResults = requestedMax;

	int32 offset = 0;
	int32 requestedOffset;
	if (query.FindInt32("offset", &requestedOffset) == B_OK)
		offset = requestedOffset;

	return fWriteDataBase->Search(queryString, offset, maxResults, reply);
}


bool
FullTextAnalyser::_InterestingEntry(const entry_ref& ref)
{
	if (_IsInIndexDirectory(ref))
		return false;

	{
		BFile file(&ref, B_READ_ONLY);
		off_t size;
		if (file.InitCheck() != B_OK || file.GetSize(&size) != B_OK
			|| size > kMaxIndexableFileSize)
			return false;
	}

	// Plain text is always indexable content on its own - no translator can
	// even produce B_TRANSLATOR_TEXT from it, so asking BTranslatorRoster to
	// Identify() it here would only probe every registered translator (every
	// image codec included) for nothing, which is not just wasteful but
	// corrupted heap memory when fed a source file this way (see #47).
	if (_IsPlainText(ref))
		return true;

	identify_cookie* cookie = new(std::nothrow) identify_cookie;
	if (cookie == NULL)
		return false;
	cookie->source = new(std::nothrow) BFile(&ref, B_READ_ONLY);
	if (cookie->source == NULL || cookie->source->InitCheck() != B_OK) {
		cleanup_identify(cookie);
		return false;
	}

	status_t status = run_with_timeout(do_identify, cookie, cleanup_identify,
		kIdentifyTimeout);
	if (status == B_TIMED_OUT) {
		// cookie now belongs to the still-running helper thread; don't
		// touch it here.
		return false;
	}
	// The actual Translate() call in _QueueTranslated() re-identifies the
	// file itself rather than reusing this result, so this is otherwise
	// unused - logged only so a future #47-style translator crash (see
	// #68) names its culprit directly in the log, instead of needing a
	// manual bisection of ~20 loaded translator add-ons after the fact.
	if (status == B_OK) {
		STRACE("identified %s as \"%s\" (translator %" B_PRId32 ")\n",
			ref.name, cookie->info.name, (int32)cookie->info.translator);
	}
	cleanup_identify(cookie);
	return status == B_OK;
}


bool
FullTextAnalyser::_IsPlainText(const entry_ref& ref)
{
	BNode node(&ref);
	char mimeType[B_MIME_TYPE_LENGTH];
	BNodeInfo nodeInfo(&node);

	// MIME types compare case-insensitively per BMimeType's own documented
	// equality rule (see #49).
	return node.InitCheck() == B_OK && nodeInfo.GetType(mimeType) == B_OK
		&& strncasecmp(mimeType, "text/", 5) == 0;
}


bool
FullTextAnalyser::_QueueTranslated(const entry_ref& ref)
{
	BPath path(&ref);

	translate_cookie* cookie = new(std::nothrow) translate_cookie;
	if (cookie == NULL)
		return false;
	cookie->source = new(std::nothrow) BFile(path.Path(), B_READ_ONLY);
	if (cookie->source == NULL || cookie->source->InitCheck() != B_OK) {
		STRACE("Can't open inFile for %s\n", path.Path());
		cleanup_translate(cookie);
		return false;
	}

	// Unique per document - more than one translated document can be
	// pending at once between here and the Commit() that actually indexes
	// it, so no two temp files may share a path. The node ref is already
	// unique and available without adding a counter to track.
	node_ref nodeRef;
	if (cookie->source->GetNodeRef(&nodeRef) != B_OK) {
		cleanup_translate(cookie);
		return false;
	}
	BPath tempPath(fDataBasePath);
	BString tempName;
	tempName.SetToFormat("temp_file_%" B_PRId64, (int64)nodeRef.node);
	tempPath.Append(tempName.String());

	cookie->destination = new(std::nothrow) BFile(tempPath.Path(),
		B_READ_WRITE | B_CREATE_FILE | B_ERASE_FILE);
	if (cookie->destination == NULL
		|| cookie->destination->InitCheck() != B_OK) {
		STRACE("Can't open outFile for %s\n", path.Path());
		cleanup_translate(cookie);
		return false;
	}

	status_t translateStatus = run_with_timeout(do_translate, cookie,
		cleanup_translate, kTranslateTimeout);
	if (translateStatus == B_TIMED_OUT) {
		// cookie now belongs to the still-running helper thread; must not
		// touch it (or tempPath, which it may still be writing to) here.
		return false;
	}
	cleanup_translate(cookie);
	if (translateStatus != B_OK) {
		remove(tempPath.Path());
		return false;
	}

	fWriteDataBase->AddDocumentFromContentFile(ref, tempPath);
	fPendingTempFiles.push_back(tempPath.Path());
	return true;
}


void
FullTextAnalyser::_DeletePendingTempFiles()
{
	for (unsigned int i = 0; i < fPendingTempFiles.size(); i++)
		remove(fPendingTempFiles.at(i).String());
	fPendingTempFiles.clear();
}


bool
FullTextAnalyser::_IsInIndexDirectory(const entry_ref& ref)
{
	BPath path(&ref);
	if (BString(path.Path()).FindFirst(fDataBasePath.Path()) == 0)
		return true;

	if (BString(path.Path()).FindFirst("/boot/system/cache/tmp") == 0)
		return true;

	return false;
}


FullTextAddOn::FullTextAddOn(image_id id, const char* name)
	:
	IndexServerAddOn(id, name)
{
	
}


FileAnalyser*
FullTextAddOn::CreateFileAnalyser(const BVolume& volume)
{
	return new (std::nothrow)FullTextAnalyser(Name(), volume);
}


extern "C" IndexServerAddOn* (instantiate_index_server_addon)(image_id id,
	const char* name)
{
	return new (std::nothrow)FullTextAddOn(id, name);
}
