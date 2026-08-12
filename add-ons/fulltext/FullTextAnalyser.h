/*
 * Copyright 2010, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Clemens Zeidler <haiku@clemens-zeidler.de>
 */
#ifndef FULL_TEXT_ANALYSER_H
#define FULL_TEXT_ANALYSER_H


#include "IndexServerAddOn.h"

#include <String.h>
#include <Path.h>

#include <vector>


class CLuceneWriteDataBase;


const char* kFullTextDirectory = "FullTextAnalyser";

//! Files larger than this are skipped rather than handed to a translator;
//! keeps a single oversized file (e.g. a video misidentified by a lenient
//! translator) from dominating the indexing queue.
const off_t kMaxIndexableFileSize = 32 * 1024 * 1024;


class FullTextAnalyser : public FileAnalyser {
public:
								FullTextAnalyser(BString name,
									const BVolume& volume);
								~FullTextAnalyser();

			status_t			InitCheck();

			void				AnalyseEntry(const entry_ref& ref);
			void				DeleteEntry(const entry_ref& ref);
			void				MoveEntry(const entry_ref& oldRef,
									const entry_ref& newRef);
			void				LastEntry();

			status_t			HandleQuery(const BMessage& query,
									BMessage& reply);

private:
	inline	bool				_InterestingEntry(const entry_ref& ref);
	inline	bool				_IsInIndexDirectory(const entry_ref& ref);
	inline	bool				_IsPlainText(const entry_ref& ref);

			//! Translates ref to a temp plain-text file next to the
			//! index (kTranslateTimeout-bounded) and queues it for
			//! indexing. The temp path is remembered in
			//! fPendingTempFiles and removed once the next Commit()
			//! has consumed it.
			bool				_QueueTranslated(const entry_ref& ref);
			void				_DeletePendingTempFiles();
			//! Logs entries whose AnalyseEntry() took longer than
			//! kSlowEntryThreshold - see its call sites' comment.
			void				_ReportSlowEntry(const entry_ref& ref,
									bigtime_t start, const char* outcome);

			CLuceneWriteDataBase*	fWriteDataBase;
			BPath				fDataBasePath;

			uint32				fNUncommited;
			std::vector<BString>	fPendingTempFiles;
};


class FullTextAddOn : public IndexServerAddOn {
public:
								FullTextAddOn(image_id id, const char* name);

			FileAnalyser*		CreateFileAnalyser(const BVolume& volume);
};

#endif
