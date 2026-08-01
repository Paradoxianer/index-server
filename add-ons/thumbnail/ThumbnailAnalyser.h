/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef THUMBNAIL_ANALYSER_H
#define THUMBNAIL_ANALYSER_H


#include "IndexServerAddOn.h"


class ThumbnailAnalyser : public FileAnalyser {
public:
								ThumbnailAnalyser(BString name,
									const BVolume& volume);

			status_t			InitCheck();

			void				AnalyseEntry(const entry_ref& ref);

private:
			bool				_IsSupportedImage(const entry_ref& ref);
};


class ThumbnailAddOn : public IndexServerAddOn {
public:
								ThumbnailAddOn(image_id id, const char* name);

			FileAnalyser*		CreateFileAnalyser(const BVolume& volume);
};

#endif
