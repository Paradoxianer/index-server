/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "CLuceneSearcher.h"

#include <stdlib.h>

#include <CLucene.h>

#include "CLuceneDataBase.h"


using namespace lucene::analysis::standard;
using namespace lucene::document;
using namespace lucene::index;
using namespace lucene::queryParser;
using namespace lucene::search;


CLuceneSearcher::CLuceneSearcher(const BPath& databasePath)
	:
	fDataBasePath(databasePath),
	fIndexExists(false)
{
	try {
		fIndexExists = IndexReader::indexExists(fDataBasePath.Path());
	} catch (CLuceneError&) {
		fIndexExists = false;
	}
}


CLuceneSearcher::~CLuceneSearcher()
{
}


bool
CLuceneSearcher::IndexExists() const
{
	return fIndexExists;
}


status_t
CLuceneSearcher::Search(const BString& queryString,
	std::vector<search_result>& results, int32 maxResults)
{
	if (!fIndexExists)
		return B_ENTRY_NOT_FOUND;

	IndexReader* reader = NULL;
	IndexSearcher* searcher = NULL;
	Query* query = NULL;
	Hits* hits = NULL;
	wchar_t* wQuery = NULL;
	status_t status = B_ERROR;

	try {
		reader = IndexReader::open(fDataBasePath.Path());
		searcher = new IndexSearcher(reader);

		size_t length = queryString.Length();
		wQuery = new wchar_t[length + 1];
		size_t converted = mbstowcs(wQuery, queryString.String(), length + 1);
		if (converted != (size_t)-1) {
			StandardAnalyzer analyzer;
			query = QueryParser::parse(wQuery, kContentsField, &analyzer);

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

				search_result result;
				result.path = path;
				result.score = hits->score(i);
				results.push_back(result);
			}
		}
		status = B_OK;
	} catch (CLuceneError&) {
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
