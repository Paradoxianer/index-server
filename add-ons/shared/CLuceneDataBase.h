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

#include <Locker.h>
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

			status_t			AddDocument(const entry_ref& ref);
			status_t			RemoveDocument(const entry_ref& ref);
			status_t			Commit();

			//! Indexes already-extracted plain text under ref's path,
			//! bypassing the BTranslatorRoster/queue pipeline entirely -
			//! for analysers that produce text themselves (e.g. a mail
			//! body) instead of pointing at a file BTranslatorRoster can
			//! convert. Commits immediately rather than joining the
			//! regular queue/Commit() cycle.
			status_t			AddDocumentWithText(const entry_ref& ref,
								const BString& text);

private:
			IndexWriter*		_OpenIndexWriter();
			IndexReader*		_OpenIndexReader();

			bool				_RemoveDocuments(std::vector<entry_ref>& docs);
			bool				_RemoveDocument(wchar_t* doc,
									IndexReader* reader);

			bool				_IndexDocument(const entry_ref& ref);

			BPath				fDataBasePath;

			BPath				fTempPath;

			std::vector<entry_ref>	fAddQueue;
			std::vector<entry_ref>	fDeleteQueue;

			StandardAnalyzer	fStandardAnalyzer;

			IndexWriter*		fIndexWriter;

			// Live monitoring and catch-up each run their own
			// FullTextAnalyser/CLuceneWriteDataBase instance for the same
			// volume, on separate threads, both pointed at the same
			// on-disk CLucene directory. CLucene 2.x's write.lock only
			// guards against two writers; a reader from one instance
			// racing a commit from the other can still tear down
			// mid-replaced segment files, which throws out of a CLucene
			// destructor and aborts the process (C++11 destructors are
			// implicitly noexcept). One process-wide lock around all
			// actual CLucene I/O closes that race.
			static	BLocker		sCLuceneLock;
};

#endif
