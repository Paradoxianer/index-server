/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef MAIL_ANALYSER_H
#define MAIL_ANALYSER_H


#include "IndexServerAddOn.h"

#include <Path.h>


class CLuceneWriteDataBase;


class MailAnalyser : public FileAnalyser {
public:
								MailAnalyser(BString name,
									const BVolume& volume);
	virtual						~MailAnalyser();

			status_t			InitCheck();

			void				AnalyseEntry(const entry_ref& ref);
			void				DeleteEntry(const entry_ref& ref);
			void				LastEntry();

private:
			bool				_IsMailFile(const entry_ref& ref);
			//! Constructs fWriteDataBase on first use rather than in the
			//! constructor - see its call sites' comment.
			CLuceneWriteDataBase*	_WriteDataBase();

			BPath				fDataBasePath;
			CLuceneWriteDataBase*	fWriteDataBase;
};


class MailAddOn : public IndexServerAddOn {
public:
								MailAddOn(image_id id, const char* name);

			FileAnalyser*		CreateFileAnalyser(const BVolume& volume);
};

#endif
