/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef EXIF_ANALYSER_H
#define EXIF_ANALYSER_H


#include "IndexServerAddOn.h"


class ExifAnalyser : public FileAnalyser {
public:
								ExifAnalyser(BString name,
									const BVolume& volume);

			status_t			InitCheck();

			void				AnalyseEntry(const entry_ref& ref);

private:
			bool				_IsSupportedImage(const entry_ref& ref);
};


class ExifAddOn : public IndexServerAddOn {
public:
								ExifAddOn(image_id id, const char* name);

			FileAnalyser*		CreateFileAnalyser(const BVolume& volume);
};

#endif
