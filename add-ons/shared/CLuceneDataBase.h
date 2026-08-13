/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */
#ifndef CLUCENE_DATA_BASE_H
#define CLUCENE_DATA_BASE_H


#include <vector>

#include <Message.h>
#include <Path.h>
#include <String.h>

#include "TextDataBase.h"

#include <CLucene.h>


using namespace lucene::index;
using namespace lucene::analysis::standard;


// Shared with the search app, which reads these same fields back out.
const TCHAR* const kContentsField = _T("contents");
const TCHAR* const kPathField = _T("path");


class CLuceneWriteDataBase : public TextWriteDataBase {
public:
								CLuceneWriteDataBase(const BPath& databasePath);
								~CLuceneWriteDataBase();

			status_t			InitCheck();

			//! Queues ref for indexing from its own file content.
			status_t			AddDocument(const entry_ref& ref);

			//! Like AddDocument(), but reads content from contentPath
			//! instead of ref's own file - for callers that extract or
			//! convert content themselves (e.g. FullTextAnalyser
			//! translating a non-text file to a temp text file first)
			//! rather than pointing at something this class can read
			//! directly. Queued and committed together with plain
			//! AddDocument() entries; this class never touches or deletes
			//! contentPath - the caller owns its lifecycle.
			status_t			AddDocumentFromContentFile(
									const entry_ref& ref,
									const BPath& contentPath);

			status_t			RemoveDocument(const entry_ref& ref);
			status_t			Commit();

			//! Indexes already-extracted plain text under ref's path,
			//! bypassing the queue pipeline entirely - for analysers that
			//! produce text themselves (e.g. a mail body) instead of
			//! pointing at any file at all. Commits immediately rather
			//! than joining the regular queue/Commit() cycle.
			status_t			AddDocumentWithText(const entry_ref& ref,
								const BString& text);

			//! Runs a free-text query against the on-disk index and appends
			//! matches [offset, offset + maxResults) - already ranked by
			//! CLucene, so this is a plain slice, not a re-sort - to
			//! \a reply as repeated "refs"/"scores" fields (same order),
			//! plus a "totalHits" count of how many matches exist in
			//! total (for callers to know whether a later page would
			//! return anything). Shares the CLucene file lock with the
			//! write path, so a search can't tear down a commit that's
			//! replacing segment files underneath it.
			status_t			Search(const BString& queryString,
								int32 offset, int32 maxResults,
								BMessage& reply);

private:
			//! A queued document and where to actually read its content
			//! from - ref's own path (AddDocument()) or an alternate path
			//! supplied by the caller (AddDocumentFromContentFile()).
			struct QueuedDocument {
				entry_ref		ref;
				BPath			contentPath;
			};

			IndexWriter*		_OpenIndexWriter();
			IndexReader*		_OpenIndexReader();

			bool				_RemoveDocuments(std::vector<entry_ref>& docs);
			bool				_RemoveDocument(wchar_t* doc,
									IndexReader* reader);

			status_t			_QueueDocument(const entry_ref& ref,
									const BPath& contentPath);
			bool				_IndexDocument(const QueuedDocument& doc);
			bool				_AddDocumentFromFile(const char* contentPath,
									const wchar_t* wPath);

			BPath				fDataBasePath;

			std::vector<QueuedDocument>	fAddQueue;
			std::vector<entry_ref>	fDeleteQueue;

			StandardAnalyzer	fStandardAnalyzer;

			IndexWriter*		fIndexWriter;
};

#endif
