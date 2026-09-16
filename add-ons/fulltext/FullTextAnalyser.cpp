/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */
#include "FullTextAnalyser.h"

#include <string.h>
#include <strings.h>

#include <File.h>
#include <Mime.h>
#include <Node.h>
#include <NodeInfo.h>
#include <String.h>
#include <TranslatorFormats.h>

#include "CLuceneDataBase.h"
#include "IndexServerPrivate.h"
#include "RunTranslatorHelper.h"
#include "TranslatorMimeCache.h"


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


FullTextAnalyser::FullTextAnalyser(BString name, const BVolume& volume)
	:
	FileAnalyser(name, volume),

	fWriteDataBase(NULL),
	fNUncommited(0)
{
	fDataBasePath = volume_index_server_directory(volume);
	fDataBasePath.Append(kFullTextDirectory);

	// fWriteDataBase is deliberately not constructed here - see
	// _WriteDataBase()'s comment. This constructor runs synchronously,
	// once per volume, from IndexServer::ReadyToRun() -> AddVolume() for
	// every mounted volume at once, before the app can process any other
	// message (including a quit request). CLuceneWriteDataBase's own
	// constructor creates a directory and takes a flock() - fine for a
	// healthy volume, but there is nothing bounding how long that can
	// take against a slow or misbehaving filesystem driver (a flaky
	// removable FAT/FAT32 volume, for instance), and unlike translator
	// calls (see RunTranslatorHelper.h) this had no timeout at all - a single
	// bad volume could hang index_server's startup entirely, which,
	// depending on what else in the boot sequence waits on it, can look
	// like the whole system failing to boot.
}


FullTextAnalyser::~FullTextAnalyser()
{
	_DeletePendingTempFiles();
	delete fWriteDataBase;
}


status_t
FullTextAnalyser::InitCheck()
{
	return fDataBasePath.InitCheck();
}


// Constructs fWriteDataBase on first actual use (the first real entry_ref
// this analyser is asked to do something with) instead of eagerly in the
// constructor - see its comment. Everything that touches fWriteDataBase
// goes through this instead of the member directly.
CLuceneWriteDataBase*
FullTextAnalyser::_WriteDataBase()
{
	if (fWriteDataBase == NULL)
		fWriteDataBase = new CLuceneWriteDataBase(fDataBasePath);
	return fWriteDataBase;
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
		_WriteDataBase()->AddDocument(ref);
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
// or the shared CLucene write lock, all of which can each individually take
// several seconds under load - delays every Progress()
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
	_WriteDataBase()->RemoveDocument(ref);
}


void
FullTextAnalyser::MoveEntry(const entry_ref& oldRef, const entry_ref& newRef)
{
	if (!_InterestingEntry(newRef))
		return;
	STRACE("FullTextAnalyser MoveEntry: %s to %s\n", oldRef.name, newRef.name);
	_WriteDataBase()->RemoveDocument(oldRef);
	AnalyseEntry(newRef);
}


void
FullTextAnalyser::LastEntry()
{
	// Checked directly (not via _WriteDataBase()) - nothing could be
	// queued without going through that accessor first, so if it's still
	// NULL there is truly nothing to commit, and constructing it here
	// just to immediately no-op would undo the point of deferring it in
	// the first place (a volume with nothing to analyse would otherwise
	// still pay for the directory/lock setup on every catch up run).
	if (fWriteDataBase != NULL)
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

	return _WriteDataBase()->Search(queryString, offset, maxResults, reply);
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

	// A freshly-created file (a temp file from an atomic save - e.g. this
	// project's own build log, the exact reproduction that led to this)
	// commonly has no MIME type set yet, so _IsPlainText()'s attribute
	// check alone would miss it and this would fall through to
	// BTranslatorRoster below for content that's actually just text.
	// update_mime_info() sniffs and fills in a missing type using the
	// Storage Kit's own content-sniffer rules (what "mimeset" wraps) -
	// entirely separate from, and much simpler than, BTranslatorRoster,
	// so this doesn't touch the translator machinery at all.
	// B_UPDATE_MIME_INFO_NO_FORCE leaves an already-set type alone.
	BPath path(&ref);
	update_mime_info(path.Path(), 0, 1, B_UPDATE_MIME_INFO_NO_FORCE);

	// Plain text is always indexable content on its own - no translator can
	// even produce B_TRANSLATOR_TEXT from it, so asking BTranslatorRoster to
	// Identify() it here would only probe every registered translator (every
	// image codec included) for nothing, which is not just wasteful but
	// corrupted heap memory when fed a source file this way (see #47).
	if (_IsPlainText(ref))
		return true;

	// Only bother asking BTranslatorRoster to Identify() this at all if some
	// installed translator actually claims to read this MIME type - an
	// object file, a stripped binary, a browser's sqlite journal are never
	// going to translate to text no matter which translator looks at them,
	// and having one look anyway is exactly how #27's crashes happened.
	// This replaces the previous NUL-byte "looks like binary" heuristic,
	// which - being blind to what's actually installed - rejected true
	// translator-eligible content just as readily as genuine junk (a real
	// image is exactly as "binary" as a stripped ELF by that heuristic).
	{
		BNode node(&ref);
		char mimeType[B_MIME_TYPE_LENGTH];
		BNodeInfo nodeInfo(&node);
		if (node.InitCheck() != B_OK || nodeInfo.GetType(mimeType) != B_OK
				|| !translator_supports_mime_type(mimeType,
					B_TRANSLATOR_TEXT)) {
			return false;
		}
	}

	status_t status = run_translator_helper(path.Path(), NULL,
		kIdentifyTimeout);
	STRACE("_InterestingEntry %s: run_translator_helper identify status=%"
		B_PRId32 "\n", ref.name, (int32)status);
	// Which translator claimed the file isn't visible here anymore now
	// that Identify() runs isolated in its own team (see
	// RunTranslatorHelper.h) - a repeatedly failing MIME type is still
	// diagnosable by testing installed translators individually, just not
	// from this log line the way it used to be.
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

	// Unique per document - more than one translated document can be
	// pending at once between here and the Commit() that actually indexes
	// it, so no two temp files may share a path. The node ref is already
	// unique and available without adding a counter to track.
	node_ref nodeRef;
	{
		BNode node(&ref);
		if (node.InitCheck() != B_OK || node.GetNodeRef(&nodeRef) != B_OK)
			return false;
	}
	BPath tempPath(fDataBasePath);
	BString tempName;
	tempName.SetToFormat("temp_file_%" B_PRId64, (int64)nodeRef.node);
	tempPath.Append(tempName.String());

	// TranslateHelper.cpp only ever creates tempPath itself, by rename()
	// on success - a timeout or any other failure leaves nothing there, so
	// there's nothing to clean up in either failure case here.
	status_t status = run_translator_helper(path.Path(), tempPath.Path(),
		kTranslateTimeout);
	STRACE("_QueueTranslated %s: run_translator_helper translate status=%"
		B_PRId32 "\n", ref.name, (int32)status);
	if (status != B_OK)
		return false;

	_WriteDataBase()->AddDocumentFromContentFile(ref, tempPath);
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
