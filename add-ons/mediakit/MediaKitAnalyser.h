/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef MEDIA_KIT_ANALYSER_H
#define MEDIA_KIT_ANALYSER_H


#include "IndexServerAddOn.h"


class MediaKitAnalyser : public FileAnalyser {
public:
								MediaKitAnalyser(BString name,
									const BVolume& volume);

			status_t			InitCheck();

			void				AnalyseEntry(const entry_ref& ref);

private:
			bool				_IsSupportedMedia(const entry_ref& ref);
};


class MediaKitAddOn : public IndexServerAddOn {
public:
								MediaKitAddOn(image_id id, const char* name);

			FileAnalyser*		CreateFileAnalyser(const BVolume& volume);
};

#endif
