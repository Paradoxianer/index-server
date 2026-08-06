/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "ExifAnalyser.h"

#include <new>
#include <string.h>
#include <strings.h>

#include <File.h>
#include <Mime.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Path.h>
#include <String.h>

#include <libexif/exif-data.h>

#include "RunWithTimeout.h"


// libexif is meant for exactly this (parsing untrusted, possibly malformed
// EXIF blocks) and is far more hardened than the ad-hoc translators this
// project already had to time-bound, but a corrupt file is still a corrupt
// file; keep the same defensive pattern used for TagLib and BTranslatorRoster
// rather than making this one analyser the exception.
static const bigtime_t kExifReadTimeout = 5 * 1000000;


namespace {


struct exif_read_cookie {
	BString	path;
	BString	make;
	BString	model;
	BString	dateTimeOriginal;
	BString	width;
	BString	height;
	bool	hasData;
};


status_t
do_read_exif(void* data)
{
	exif_read_cookie* cookie = (exif_read_cookie*)data;
	ExifData* exifData = exif_data_new_from_file(cookie->path.String());
	if (exifData == NULL)
		return B_OK;

	char buffer[1024];
	ExifContent* ifd0 = exifData->ifd[EXIF_IFD_0];
	ExifContent* ifdExif = exifData->ifd[EXIF_IFD_EXIF];

	if (exif_content_get_value(ifd0, EXIF_TAG_MAKE, buffer, sizeof(buffer))
			!= NULL) {
		cookie->make = buffer;
	}
	if (exif_content_get_value(ifd0, EXIF_TAG_MODEL, buffer, sizeof(buffer))
			!= NULL) {
		cookie->model = buffer;
	}
	if (exif_content_get_value(ifdExif, EXIF_TAG_DATE_TIME_ORIGINAL, buffer,
			sizeof(buffer)) != NULL) {
		cookie->dateTimeOriginal = buffer;
	}
	if (exif_content_get_value(ifdExif, EXIF_TAG_PIXEL_X_DIMENSION, buffer,
			sizeof(buffer)) != NULL) {
		cookie->width = buffer;
	}
	if (exif_content_get_value(ifdExif, EXIF_TAG_PIXEL_Y_DIMENSION, buffer,
			sizeof(buffer)) != NULL) {
		cookie->height = buffer;
	}

	cookie->hasData = true;
	exif_data_unref(exifData);
	return B_OK;
}


void
cleanup_read_exif(void* data)
{
	delete (exif_read_cookie*)data;
}


}	// namespace


ExifAnalyser::ExifAnalyser(BString name, const BVolume& volume)
	:
	FileAnalyser(name, volume)
{
}


status_t
ExifAnalyser::InitCheck()
{
	return B_OK;
}


bool
ExifAnalyser::_IsSupportedImage(const entry_ref& ref)
{
	BNode node(&ref);
	if (node.InitCheck() != B_OK)
		return false;

	BNodeInfo nodeInfo(&node);
	char mimeType[B_MIME_TYPE_LENGTH];
	if (nodeInfo.GetType(mimeType) != B_OK)
		return false;

	// libexif reads EXIF out of JPEG's APP1 marker or a raw TIFF/EXIF blob;
	// most other image formats never carry EXIF at all, so there is no
	// point handing them to it. MIME types compare case-insensitively per
	// BMimeType's own documented equality rule (see #49).
	return strcasecmp(mimeType, "image/jpeg") == 0
		|| strcasecmp(mimeType, "image/tiff") == 0;
}


void
ExifAnalyser::AnalyseEntry(const entry_ref& ref)
{
	if (!_IsSupportedImage(ref))
		return;

	BPath path(&ref);

	exif_read_cookie* cookie = new(std::nothrow) exif_read_cookie;
	if (cookie == NULL)
		return;
	cookie->path = path.Path();
	cookie->hasData = false;

	status_t status = run_with_timeout(do_read_exif, cookie,
		cleanup_read_exif, kExifReadTimeout);
	if (status == B_TIMED_OUT) {
		// cookie now belongs to the still-running helper thread; must not
		// touch it here.
		return;
	}
	if (status != B_OK || !cookie->hasData) {
		delete cookie;
		return;
	}

	BFile file(&ref, B_READ_ONLY);
	if (file.InitCheck() == B_OK) {
		if (cookie->make.Length() > 0) {
			file.WriteAttr("EXIF:Make", B_STRING_TYPE, 0,
				cookie->make.String(), cookie->make.Length());
		}
		if (cookie->model.Length() > 0) {
			file.WriteAttr("EXIF:Model", B_STRING_TYPE, 0,
				cookie->model.String(), cookie->model.Length());
		}
		if (cookie->dateTimeOriginal.Length() > 0) {
			file.WriteAttr("EXIF:DateTimeOriginal", B_STRING_TYPE, 0,
				cookie->dateTimeOriginal.String(),
				cookie->dateTimeOriginal.Length());
		}
		if (cookie->width.Length() > 0) {
			file.WriteAttr("EXIF:Width", B_STRING_TYPE, 0,
				cookie->width.String(), cookie->width.Length());
		}
		if (cookie->height.Length() > 0) {
			file.WriteAttr("EXIF:Height", B_STRING_TYPE, 0,
				cookie->height.String(), cookie->height.Length());
		}
	}
	delete cookie;
}


ExifAddOn::ExifAddOn(image_id id, const char* name)
	:
	IndexServerAddOn(id, name)
{
}


FileAnalyser*
ExifAddOn::CreateFileAnalyser(const BVolume& volume)
{
	return new (std::nothrow) ExifAnalyser(Name(), volume);
}


extern "C" IndexServerAddOn* (instantiate_index_server_addon)(image_id id,
	const char* name)
{
	return new (std::nothrow) ExifAddOn(id, name);
}
