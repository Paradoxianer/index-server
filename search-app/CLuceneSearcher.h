/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef CLUCENE_SEARCHER_H
#define CLUCENE_SEARCHER_H


#include <vector>

#include <Path.h>
#include <String.h>


struct search_result {
	BString		path;
	float		score;
};


//! Read-only counterpart to CLuceneWriteDataBase; opens an existing
//! per-volume index and runs free-text queries against it.
class CLuceneSearcher {
public:
								CLuceneSearcher(const BPath& databasePath);
								~CLuceneSearcher();

			bool				IndexExists() const;

			//! Appends up to maxResults matches to \a results (doesn't
			//! clear it first, so callers can merge results from several
			//! per-volume searchers).
			status_t			Search(const BString& queryString,
									std::vector<search_result>& results,
									int32 maxResults = 100);

private:
			BPath				fDataBasePath;
			bool				fIndexExists;
};


#endif // CLUCENE_SEARCHER_H
