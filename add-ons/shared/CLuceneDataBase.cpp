/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		based on previous work of Ankur Sethi
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */

#include "CLuceneDataBase.h"

#include <new>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <Directory.h>
#include <Entry.h>
#include <UnicodeChar.h>


#define DEBUG_CLUCENE_DATABASE
#ifdef DEBUG_CLUCENE_DATABASE
#include <stdio.h>
#	define STRACE(x...) printf("FT: " x)
#else
#	define STRACE(x...) ;
#endif


using namespace lucene::document;
using namespace lucene::queryParser;
using namespace lucene::search;
using namespace lucene::util;


const uint8 kCluceneTries = 10;

// CLucene's own default (10000) is tuned for typical documents and is
// reached by any reasonably large real text file (a log, a big source
// file) - addDocument() then throws, which _IndexDocument()/Commit()
// treated as a retryable failure it never was: retried kCluceneTries
// times against the exact same oversized content, failing identically
// every time, then abandoning the rest of that Commit() batch (see the
// fAddQueue.clear() note below). Generous but still bounded, in the same
// spirit as FullTextAnalyser's kMaxIndexableFileSize.
const int32 kMaxFieldLength = 1000000;

// See FullTextAnalyser.cpp's kSlowEntryThreshold - same reasoning, kept in
// sync deliberately.
const bigtime_t kSlowThreshold = 200 * 1000;


namespace {


// MailAnalyser and FullTextAnalyser both point their CLuceneWriteDataBase
// at the same on-disk directory (same kFullTextDirectory constant, defined
// separately in each) - but they're two separately loaded add-on images,
// each with its own copy of any static/in-process lock (a BLocker used to
// live here, guarding only against races within one image's own instances,
// e.g. FullTextAnalyser's live vs. catch-up threads - it never actually
// serialized FullTextAnalyser against MailAnalyser, see #66). flock() is
// enforced by the kernel on the underlying inode, not by process memory,
// so a lock file inside the shared directory actually closes that gap
// regardless of which image - or even which process - is asking.
class CLuceneFileLock {
public:
	CLuceneFileLock(const BPath& dataBasePath)
		:
		fFd(-1)
	{
		BPath lockPath(dataBasePath);
		lockPath.Append("index_server.lock");
		fFd = open(lockPath.Path(), O_CREAT | O_RDWR, 0644);
		if (fFd >= 0)
			flock(fFd, LOCK_EX);
	}

	~CLuceneFileLock()
	{
		if (fFd >= 0) {
			flock(fFd, LOCK_UN);
			close(fFd);
		}
	}

private:
	int	fFd;
};


// create_directory() on the not-yet-existing target itself can't be
// guarded by a lock file living inside that same directory (chicken-and-
// egg - CLuceneFileLock above assumes fDataBasePath already exists).
// MailAnalyser and FullTextAnalyser can both construct their
// CLuceneWriteDataBase for the same not-yet-existing directory at nearly
// the same instant during add-on registration; on real hardware that raced
// BFS's own block allocator into "PANIC: blocks already set!" while
// building the new directory's B+tree (most likely a BFS locking bug, but
// trivially avoidable from here). The lock file instead lives in
// dataBasePath's *parent*, which - unlike the target - is reliably created
// ahead of time (IndexServerSettings/VolumeWatcher write their own files
// there during startup, before any analyser is constructed).
class CLuceneDirectoryCreateLock {
public:
	CLuceneDirectoryCreateLock(const BPath& dataBasePath)
		:
		fFd(-1)
	{
		BPath lockPath;
		if (dataBasePath.GetParent(&lockPath) != B_OK)
			return;
		lockPath.Append("index_server_dircreate.lock");
		fFd = open(lockPath.Path(), O_CREAT | O_RDWR, 0644);
		if (fFd >= 0)
			flock(fFd, LOCK_EX);
	}

	~CLuceneDirectoryCreateLock()
	{
		if (fFd >= 0) {
			flock(fFd, LOCK_UN);
			close(fFd);
		}
	}

private:
	int	fFd;
};


}	// namespace


// mbstowcs() depends on the process's current locale to decode multi-byte
// sequences, but a process starts in the locale-less "POSIX" locale unless
// something explicitly opts in - silently failing (and thereby dropping
// the whole document from the index) on any path with so much as one
// accented character (see #56). Haiku paths are UTF-8 natively; decoding
// that with BUnicodeChar::FromUTF8() doesn't depend on any locale being
// configured at all.
wchar_t* to_wchar(const char *str)
{
	if (str == NULL)
		return NULL;

	size_t length = BUnicodeChar::UTF8StringLength(str);
	wchar_t* wStr = new(std::nothrow) wchar_t[length + 1];
	if (wStr == NULL)
		return NULL;

	const char* current = str;
	for (size_t i = 0; i < length; i++)
		wStr[i] = (wchar_t)BUnicodeChar::FromUTF8(&current);
	wStr[length] = 0;

	return wStr;
}


CLuceneWriteDataBase::CLuceneWriteDataBase(const BPath& databasePath)
	:
	fDataBasePath(databasePath),
	fIndexWriter(NULL)
{
	printf("CLuceneWriteDataBase fDataBasePath %s\n", fDataBasePath.Path());
	CLuceneDirectoryCreateLock lock(fDataBasePath);
	create_directory(fDataBasePath.Path(), 0755);
}


CLuceneWriteDataBase::~CLuceneWriteDataBase()
{
}


status_t
CLuceneWriteDataBase::InitCheck()
{

	return B_OK;
}


status_t
CLuceneWriteDataBase::AddDocument(const entry_ref& ref)
{
	return _QueueDocument(ref, BPath(&ref));
}


status_t
CLuceneWriteDataBase::AddDocumentFromContentFile(const entry_ref& ref,
	const BPath& contentPath)
{
	return _QueueDocument(ref, contentPath);
}


status_t
CLuceneWriteDataBase::_QueueDocument(const entry_ref& ref,
	const BPath& contentPath)
{
	// check if already in the queue
	for (unsigned int i = 0; i < fAddQueue.size(); i++) {
		if (fAddQueue.at(i).ref == ref)
			return B_OK;
	}
	QueuedDocument doc;
	doc.ref = ref;
	doc.contentPath = contentPath;
	fAddQueue.push_back(doc);

	return B_OK;
}


status_t
CLuceneWriteDataBase::RemoveDocument(const entry_ref& ref)
{
	// check if already in the queue
	for (unsigned int i = 0; i < fDeleteQueue.size(); i++) {
		if (fDeleteQueue.at(i) == ref)
			return B_OK;
	}
	fDeleteQueue.push_back(ref);
	return B_OK;
}


status_t
CLuceneWriteDataBase::Commit()
{
	if (fAddQueue.size() == 0 && fDeleteQueue.size() == 0)
		return B_OK;
	STRACE("Commit\n");

	// Serializes against every other CLuceneWriteDataBase instance pointed
	// at the same on-disk directory - live monitoring, catch-up, and even
	// MailAnalyser's own instances all qualify (see CLuceneFileLock's
	// comment).
	bigtime_t lockWaitStart = system_time();
	CLuceneFileLock lock(fDataBasePath);
	bigtime_t commitStart = system_time();
	if (commitStart - lockWaitStart > kSlowThreshold) {
		STRACE("Commit: waited %" B_PRId64 " ms for the CLucene file lock\n",
			(commitStart - lockWaitStart) / 1000);
	}

	// Delete any existing version of a re-added document first - CLucene
	// has no update, so refreshing a changed file's content is a delete
	// followed by a re-add.
	std::vector<entry_ref> addedRefs;
	for (unsigned int i = 0; i < fAddQueue.size(); i++)
		addedRefs.push_back(fAddQueue.at(i).ref);
	_RemoveDocuments(addedRefs);
	_RemoveDocuments(fDeleteQueue);
	fDeleteQueue.clear();

	if (fAddQueue.size() == 0)
		return B_OK;

	fIndexWriter = _OpenIndexWriter();
	if (fIndexWriter == NULL)
		return B_ERROR;

	// One document that _IndexDocument() can never index (e.g. content
	// over kMaxFieldLength) must not take the rest of this batch down with
	// it - keep going for whatever's left in the queue, only actually
	// giving up if the writer itself died (fIndexWriter went NULL inside
	// _IndexDocument()'s own reopen-and-retry loop), since every other
	// document would fail immediately too - see kMaxFieldLength's comment.
	status_t status = B_OK;
	for (unsigned int i = 0; i < fAddQueue.size(); i++) {
		if (!_IndexDocument(fAddQueue.at(i))) {
			status = B_ERROR;
			if (fIndexWriter == NULL)
				break;
		}
	}

	fAddQueue.clear();
	if (fIndexWriter != NULL) {
		fIndexWriter->close();
		delete fIndexWriter;
		fIndexWriter = NULL;
	}

	bigtime_t commitElapsed = system_time() - commitStart;
	if (commitElapsed > kSlowThreshold) {
		STRACE("Commit: took %" B_PRId64 " ms total\n",
			commitElapsed / 1000);
	}

	return status;
}


status_t
CLuceneWriteDataBase::AddDocumentWithText(const entry_ref& ref,
	const BString& text)
{
	STRACE("AddDocumentWithText %s (%ld bytes)\n", ref.name,
		(long)text.Length());

	CLuceneFileLock lock(fDataBasePath);

	std::vector<entry_ref> single;
	single.push_back(ref);
	_RemoveDocuments(single);

	fIndexWriter = _OpenIndexWriter();
	if (fIndexWriter == NULL)
		return B_ERROR;

	BPath path(&ref);
	wchar_t* wPath = to_wchar(path.Path());
	wchar_t* wText = to_wchar(text.String());
	status_t status = B_OK;
	if (wPath == NULL || wText == NULL) {
		status = B_NO_MEMORY;
	} else {
		// Document::add(Field&) stores the field by address and deletes it
		// from Document's own destructor - see the identical fix and its
		// comment in _AddDocumentFromFile() - so both fields must be heap
		// allocated here too, not stack locals.
		Document* document = new Document;
		Field* contentField = new Field(kContentsField, wText,
			Field::STORE_NO | Field::INDEX_TOKENIZED);
		document->add(*contentField);
		Field* pathField = new Field(kPathField, wPath,
			Field::STORE_YES | Field::INDEX_UNTOKENIZED);
		document->add(*pathField);

		try {
			fIndexWriter->addDocument(document);
		} catch (CLuceneError &error) {
			STRACE("CLuceneError addDocument (text) %s\n", error.what());
			status = B_ERROR;
		}
		delete document;
	}
	delete[] wPath;
	delete[] wText;

	fIndexWriter->close();
	delete fIndexWriter;
	fIndexWriter = NULL;

	return status;
}


status_t
CLuceneWriteDataBase::Search(const BString& queryString, int32 maxResults,
	BMessage& reply)
{
	bigtime_t lockWaitStart = system_time();
	CLuceneFileLock lock(fDataBasePath);
	STRACE("Search: waited %" B_PRId64 " us for the CLucene file lock\n",
		system_time() - lockWaitStart);
	bigtime_t searchStart = system_time();

	wchar_t* wQuery = to_wchar(queryString.String());
	if (wQuery == NULL)
		return B_NO_MEMORY;

	IndexReader* reader = NULL;
	IndexSearcher* searcher = NULL;
	Query* query = NULL;
	Hits* hits = NULL;
	status_t status = B_ENTRY_NOT_FOUND;

	try {
		if (IndexReader::indexExists(fDataBasePath.Path())) {
			reader = IndexReader::open(fDataBasePath.Path());
			searcher = new IndexSearcher(reader);
			query = QueryParser::parse(wQuery, kContentsField,
				&fStandardAnalyzer);
			hits = searcher->search(query);

			int32 count = (int32)hits->length();
			if (count > maxResults)
				count = maxResults;

			for (int32 i = 0; i < count; i++) {
				Document& doc = hits->doc(i);
				const TCHAR* wPath = doc.get(kPathField);
				if (wPath == NULL)
					continue;

				// wcstombs() is the same locale-dependent trap to_wchar()
				// used to fall into (see #56 and that function's comment) -
				// BUnicodeChar::ToUTF8() doesn't depend on locale at all.
				// A UTF-8 character is up to 4 bytes, so stop with enough
				// room left for one more plus the terminator.
				char path[B_PATH_NAME_LENGTH];
				char* pathEnd = path;
				for (const TCHAR* w = wPath; *w != 0; w++) {
					if (pathEnd - path >= B_PATH_NAME_LENGTH - 5)
						break;
					BUnicodeChar::ToUTF8((uint32)*w, &pathEnd);
				}
				*pathEnd = '\0';

				entry_ref ref;
				BEntry entry(path);
				if (entry.InitCheck() != B_OK || entry.GetRef(&ref) != B_OK)
					continue;

				reply.AddRef("refs", &ref);
				reply.AddFloat("scores", hits->score(i));
			}
			status = B_OK;
		}
	} catch (CLuceneError &error) {
		STRACE("CLuceneError: Search %s\n", error.what());
		status = B_ERROR;
	}

	delete[] wQuery;
	delete hits;
	delete query;
	if (searcher != NULL)
		searcher->close();
	delete searcher;
	if (reader != NULL)
		reader->close();
	delete reader;

	STRACE("Search: actual search took %" B_PRId64 " us, status %d\n",
		system_time() - searchStart, (int)status);

	return status;
}


IndexWriter*
CLuceneWriteDataBase::_OpenIndexWriter()
{
	// The CLuceneFileLock caller already holds guarantees nothing else is
	// touching this directory here - so a lock found here can only be
	// stale, left behind by a previous instance that didn't exit cleanly
	// (crash, kill). Left alone, such a lock blocks every future write
	// forever.
	if (IndexReader::isLocked(fDataBasePath.Path())) {
		STRACE("stale write lock found, clearing %s\n", fDataBasePath.Path());
		IndexReader::unlock(fDataBasePath.Path());
	}

	IndexWriter* writer = NULL;
	for (int i = 0; i < kCluceneTries; i++) {
		try {
			bool createIndex = true;
			if (IndexReader::indexExists(fDataBasePath.Path()))
				createIndex = false;

			writer = new IndexWriter(fDataBasePath.Path(),
				&fStandardAnalyzer, createIndex);
			if (writer) {
				writer->setMaxFieldLength(kMaxFieldLength);
				break;
			}
		} catch (CLuceneError &error) {
			STRACE("CLuceneError: _OpenIndexWriter %s\n", error.what());
			delete writer;
			writer = NULL;
		}
	}
	return writer;
}


IndexReader*
CLuceneWriteDataBase::_OpenIndexReader()
{
	IndexReader* reader = NULL;

	BEntry entry(fDataBasePath.Path(), NULL);
	if (!entry.Exists())
		return NULL;

	for (int i = 0; i < kCluceneTries; i++) {
		try {
			if (!IndexReader::indexExists(fDataBasePath.Path()))
				return NULL;

			reader = IndexReader::open(fDataBasePath.Path());
			if (reader)
				break;
		} catch (CLuceneError &error) {
			STRACE("CLuceneError: _OpenIndexReader %s\n", error.what());
			delete reader;
			reader = NULL;
		}
	}

	return reader;
}


bool
CLuceneWriteDataBase::_RemoveDocuments(std::vector<entry_ref>& docs)
{
	IndexReader *reader = NULL;
	reader = _OpenIndexReader();
	if (!reader)
		return false;
	bool status = false;

	for (unsigned int i = 0; i < docs.size(); i++) {
		BPath path(&docs.at(i));
		wchar_t* wPath = to_wchar(path.Path());
		if (wPath == NULL)
			continue;
		
		for (int i = 0; i < kCluceneTries; i++) {
			status = _RemoveDocument(wPath, reader);
			if (status)
				break;
			reader->close();
			delete reader;
			reader = _OpenIndexReader();
			if (!reader) {
				status = false;
				break;
			}
		}
		delete[] wPath;

		if (!status)
			break;
	}

	reader->close();
	delete reader;

	return status;
}


bool
CLuceneWriteDataBase::_RemoveDocument(wchar_t* wPath, IndexReader* reader)
{
	try {
		Term term(kPathField, wPath);
		reader->deleteDocuments(&term);
	} catch (CLuceneError &error) {
		STRACE("CLuceneError: deleteDocuments %s\n", error.what());
		return false;
	}
	return true;
}


bool
CLuceneWriteDataBase::_AddDocumentFromFile(const char* contentPath,
	const wchar_t* wPath)
{
	bool status = true;
	for (int i = 0; i < kCluceneTries; i++) {
		// A fresh Document/Field/FileReader every attempt - addDocument()
		// throwing partway through may already have consumed some or all
		// of a previous attempt's reader, so retrying with the same one
		// risks indexing empty/truncated content instead of actually
		// retrying. Document::add(Field&) stores the field by address and
		// deletes it from Document's own destructor - not documented, but
		// confirmed the hard way (a real crash) when these were stack
		// allocated - so both fields must be heap allocated here. Declared
		// outside the try block (unlike the Field/FileReader below) so
		// every catch clause can safely delete it regardless of how far
		// construction got.
		Document* document = new Document;

		try {
			// The source file can vanish between being queued and being
			// read here - renamed away by an atomic editor save (the
			// crash this was found from: a ".xZQxHr"-style temp name),
			// deleted by git, etc. FileReader/FileInputStream throw a
			// raw std::ios_base::failure (not CLuceneError) when the
			// open fails, which used to propagate straight past this
			// function's catch and abort the whole server - widen the
			// catch below to cover that too.
			FileReader* fileReader = new FileReader(contentPath, "UTF-8");
			Field* contentField = new Field(kContentsField, fileReader,
				Field::STORE_NO | Field::INDEX_TOKENIZED);
			document->add(*contentField);
			Field* pathField = new Field(kPathField, wPath,
				Field::STORE_YES | Field::INDEX_UNTOKENIZED);
			document->add(*pathField);

			fIndexWriter->addDocument(document);
			STRACE("document added, retries: %i\n", i);
			delete document;
			break;
		} catch (CLuceneError &error) {
			STRACE("CLuceneError addDocument %s\n", error.what());
			delete document;
			fIndexWriter->close();
			delete fIndexWriter;
			fIndexWriter = _OpenIndexWriter();
			if (fIndexWriter == NULL) {
				status = false;
				break;
			}
		} catch (std::exception &error) {
			// Not retryable - the content is just gone - so skip this
			// document instead of looping kCluceneTries times against
			// the same missing file.
			STRACE("exception building document for %s: %s\n", contentPath,
				error.what());
			delete document;
			status = false;
			break;
		}
	}

	return status;
}


bool
CLuceneWriteDataBase::_IndexDocument(const QueuedDocument& doc)
{
	BPath path(&doc.ref);
	wchar_t* wPath = to_wchar(path.Path());
	if (wPath == NULL)
		return false;
	bool status = _AddDocumentFromFile(doc.contentPath.Path(), wPath);
	delete[] wPath;
	return status;
}
