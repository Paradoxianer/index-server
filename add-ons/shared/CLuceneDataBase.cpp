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

#include <Autolock.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <TranslatorRoster.h>

#include "RunWithTimeout.h"


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

// A hung or pathological translator must not stall the whole VolumeWorker
// thread (it processes every entry of a volume serially).
const bigtime_t kTranslateTimeout = 30 * 1000000;


namespace {


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
	return BTranslatorRoster::Default()->Translate(cookie->source, NULL, NULL,
		cookie->destination, 'TEXT');
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


BLocker CLuceneWriteDataBase::sCLuceneLock("CLucene index lock");


wchar_t* to_wchar(const char *str)
{
	if (str == NULL)
		return NULL ;

	int size = strlen(str) * sizeof(wchar_t) ;
	wchar_t *wStr = new wchar_t[size] ;

	if (mbstowcs(wStr, str, size) == -1) {
		delete[] wStr ;
		return NULL ;
	} else
		return wStr ;
}


CLuceneWriteDataBase::CLuceneWriteDataBase(const BPath& databasePath)
	:
	fDataBasePath(databasePath),
	fTempPath(databasePath),
	fIndexWriter(NULL)
{
	printf("CLuceneWriteDataBase fDataBasePath %s\n", fDataBasePath.Path());
	create_directory(fDataBasePath.Path(), 0755);

	fTempPath.Append("temp_file");
}


CLuceneWriteDataBase::~CLuceneWriteDataBase()
{
	// TODO: delete fTempPath file
}


status_t
CLuceneWriteDataBase::InitCheck()
{

	return B_OK;
}


status_t
CLuceneWriteDataBase::AddDocument(const entry_ref& ref)
{
	// check if already in the queue
	for (unsigned int i = 0; i < fAddQueue.size(); i++) {
		if (fAddQueue.at(i) == ref)
			return B_OK;
	}
	fAddQueue.push_back(ref);

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

	// Serializes against every other CLuceneWriteDataBase instance in this
	// process (see sCLuceneLock's declaration) - live monitoring and
	// catch-up for the same volume each hold their own instance pointed at
	// the same on-disk directory.
	BAutolock lock(sCLuceneLock);

	_RemoveDocuments(fAddQueue);
	_RemoveDocuments(fDeleteQueue);
	fDeleteQueue.clear();

	if (fAddQueue.size() == 0)
		return B_OK;

	fIndexWriter = _OpenIndexWriter();
	if (fIndexWriter == NULL)
		return B_ERROR;

	status_t status = B_OK;
	for (unsigned int i = 0; i < fAddQueue.size(); i++) {
		if (!_IndexDocument(fAddQueue.at(i))) {
			status = B_ERROR;
			break;
		}
	}

	fAddQueue.clear();
	fIndexWriter->close();
	delete fIndexWriter;
	fIndexWriter = NULL;

	return status;
}


status_t
CLuceneWriteDataBase::AddDocumentWithText(const entry_ref& ref,
	const BString& text)
{
	STRACE("AddDocumentWithText %s (%ld bytes)\n", ref.name,
		(long)text.Length());

	BAutolock lock(sCLuceneLock);

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
		Document* document = new Document;
		Field contentField(kContentsField, wText,
			Field::STORE_NO | Field::INDEX_TOKENIZED);
		document->add(contentField);
		Field pathField(kPathField, wPath,
			Field::STORE_YES | Field::INDEX_UNTOKENIZED);
		document->add(pathField);

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
	BAutolock lock(sCLuceneLock);

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

				char path[B_PATH_NAME_LENGTH];
				wcstombs(path, wPath, sizeof(path));

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

	return status;
}


IndexWriter*
CLuceneWriteDataBase::_OpenIndexWriter()
{
	IndexWriter* writer = NULL;
	for (int i = 0; i < kCluceneTries; i++) {
		try {
			bool createIndex = true;
			if (IndexReader::indexExists(fDataBasePath.Path()))
				createIndex = false;

			writer = new IndexWriter(fDataBasePath.Path(),
				&fStandardAnalyzer, createIndex);
			if (writer)
				break;
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
CLuceneWriteDataBase::_IndexDocument(const entry_ref& ref)
{
	BPath path(&ref);

	translate_cookie* cookie = new(std::nothrow) translate_cookie;
	if (cookie == NULL)
		return false;
	cookie->source = new(std::nothrow) BFile(path.Path(), B_READ_ONLY);
	cookie->destination = new(std::nothrow) BFile(fTempPath.Path(),
		B_READ_WRITE | B_CREATE_FILE | B_ERASE_FILE);
	if (cookie->source == NULL || cookie->destination == NULL
		|| cookie->source->InitCheck() != B_OK
		|| cookie->destination->InitCheck() != B_OK) {
		STRACE("Can't open inFile/outFile for %s\n", path.Path());
		cleanup_translate(cookie);
		return false;
	}

	status_t translateStatus = run_with_timeout(do_translate, cookie,
		cleanup_translate, kTranslateTimeout);
	if (translateStatus == B_TIMED_OUT) {
		// cookie now belongs to the still-running helper thread; must not
		// touch it (or fTempPath, which it may still be writing to) here.
		return false;
	}
	cleanup_translate(cookie);
	if (translateStatus != B_OK)
		return false;

	FileReader* fileReader = new FileReader(fTempPath.Path(), "UTF-8");
	wchar_t* wPath = to_wchar(path.Path());
	if (wPath == NULL)
		return false;

	Document *document = new Document;
	Field contentField(kContentsField, fileReader,
		Field::STORE_NO | Field::INDEX_TOKENIZED);
	document->add(contentField);
	Field pathField(kPathField, wPath,
		Field::STORE_YES | Field::INDEX_UNTOKENIZED);
	document->add(pathField);

	bool status = true;
	for (int i = 0; i < kCluceneTries; i++) {
		try {
			fIndexWriter->addDocument(document);
			STRACE("document added, retries: %i\n", i);
			break;
		} catch (CLuceneError &error) {
			STRACE("CLuceneError addDocument %s\n", error.what());
			fIndexWriter->close();
			delete fIndexWriter;
			fIndexWriter = _OpenIndexWriter();
			if (fIndexWriter == NULL) {
				status = false;
				break;
			}
		}
	}

	if (!status)
		delete document;
	delete[] wPath;
	return status;
}
