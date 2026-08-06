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
	if (!_InterestingEntry(ref))
		return;

	//STRACE("FullTextAnalyser AnalyseEntry: %s %s\n", ref.name, path.Path());
	fWriteDataBase->AddDocument(ref);

	fNUncommited++;
	if (fNUncommited > 100)
		LastEntry();
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

	return fWriteDataBase->Search(queryString, maxResults, reply);
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
	// image codec included) for nothing. See the same reasoning in
	// CLuceneWriteDataBase::_IndexDocument(), which is what actually reads
	// this content - one of those translators corrupted heap memory when fed
	// a source file this way (see #47).
	{
		BNode node(&ref);
		char mimeType[B_MIME_TYPE_LENGTH];
		BNodeInfo nodeInfo(&node);
		if (node.InitCheck() == B_OK && nodeInfo.GetType(mimeType) == B_OK
			&& strncasecmp(mimeType, "text/", 5) == 0) {
			return true;
		}
	}

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
	cleanup_identify(cookie);
	return status == B_OK;
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
