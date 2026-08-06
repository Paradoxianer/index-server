/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "ThumbnailAnalyser.h"

#include <new>
#include <string.h>
#include <strings.h>

#include <Bitmap.h>
#include <BitmapStream.h>
#include <DataIO.h>
#include <File.h>
#include <Mime.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Path.h>
#include <TranslatorFormats.h>
#include <TranslatorRoster.h>
#include <View.h>

#include "RunWithTimeout.h"


// A malformed or pathological image must not stall the whole VolumeWorker
// thread over decoding/scaling it.
const bigtime_t kThumbnailTimeout = 15 * 1000000;

// Larger than Tracker's own icon sizes so a future viewer has some room,
// small enough to stay cheap to generate and store per file.
const int32 kThumbnailSize = 128;

// Skip anything above this before even trying to decode it - a huge image
// dominating the queue is exactly the kind of thing kMaxIndexableFileSize
// already guards against for text (see FullTextAnalyser.h).
const off_t kMaxThumbnailSourceSize = 32 * 1024 * 1024;

// Tracker already reads a thumbnail straight from these attributes for
// every icon it draws (IconCache::GetNodeIcon() -> GetThumbnailFromAttr(),
// src/kits/tracker/Thumbnails.cpp) - not a new convention invented here.
// It also requires WebP specifically, and treats an existing thumbnail as
// stale unless its creation time is after the file's own modification time.
static const char* const kThumbnailAttribute = "Media:Thumbnail";
static const char* const kThumbnailCreationTimeAttribute
	= "Media:Thumbnail:CreationTime";

// Haiku's own vector icon format isn't typed "image/*" (HVIFTranslator
// registers it as this application/* type - see HVIFTranslator.cpp), even
// though it translates to B_TRANSLATOR_BITMAP just like any other image
// format here.
static const char* const kHVIFMimeType = "application/x-vnd.Haiku-icon";


namespace {


struct thumbnail_cookie {
	BString		path;
	bool		hasThumbnail;
	BMallocIO	thumbnailData;
};


status_t
do_create_thumbnail(void* data)
{
	thumbnail_cookie* cookie = (thumbnail_cookie*)data;

	BFile file(cookie->path.String(), B_READ_ONLY);
	if (file.InitCheck() != B_OK)
		return B_OK;

	BBitmapStream sourceStream;
	if (BTranslatorRoster::Default()->Translate(&file, NULL, NULL,
			&sourceStream, B_TRANSLATOR_BITMAP) != B_OK) {
		return B_OK;
	}

	BBitmap* sourceBitmap = NULL;
	if (sourceStream.DetachBitmap(&sourceBitmap) != B_OK
		|| sourceBitmap == NULL) {
		return B_OK;
	}

	BRect sourceBounds = sourceBitmap->Bounds();
	float sourceWidth = sourceBounds.Width() + 1;
	float sourceHeight = sourceBounds.Height() + 1;
	if (sourceWidth <= 0 || sourceHeight <= 0) {
		delete sourceBitmap;
		return B_OK;
	}

	// Tracker's own GetThumbnailFromAttr() (Thumbnails.cpp) imports a stored
	// thumbnail directly via BBitmap::ImportBits() with no rescale whenever
	// the requested icon size is exactly B_XXL_ICON (128, same as
	// kThumbnailSize) - it only assumes a fixed kThumbnailSize x
	// kThumbnailSize square. A canvas sized to the source's own aspect
	// ratio (e.g. 128x64 for a wide image) silently fails that import.
	// Always emit a fixed square canvas, letterboxing the scaled content
	// centered within it exactly like Tracker's own ScaleBitmap()/
	// ThumbBounds() do, so the two stay bit-compatible.
	float longSide = sourceWidth > sourceHeight ? sourceWidth : sourceHeight;
	float scale = kThumbnailSize / longSide;
	if (scale > 1)
		scale = 1; // never upscale a smaller image
	float destWidth = sourceWidth * scale;
	float destHeight = sourceHeight * scale;
	if (destWidth < 1)
		destWidth = 1;
	if (destHeight < 1)
		destHeight = 1;

	BRect canvasBounds(0, 0, kThumbnailSize - 1, kThumbnailSize - 1);
	BRect destBounds(0, 0, destWidth - 1, destHeight - 1);
	destBounds.OffsetBySelf((kThumbnailSize - destWidth) / 2.0f,
		(kThumbnailSize - destHeight) / 2.0f);

	BBitmap* destBitmap = new(std::nothrow) BBitmap(canvasBounds, B_RGBA32,
		true);
	if (destBitmap == NULL || destBitmap->InitCheck() != B_OK) {
		delete sourceBitmap;
		delete destBitmap;
		return B_OK;
	}

	BView* view = new(std::nothrow) BView(canvasBounds, "thumb",
		B_FOLLOW_NONE, B_WILL_DRAW);
	if (view == NULL) {
		delete sourceBitmap;
		delete destBitmap;
		return B_OK;
	}
	destBitmap->AddChild(view);
	destBitmap->Lock();
	view->SetLowColor(B_TRANSPARENT_COLOR);
	view->FillRect(canvasBounds, B_SOLID_LOW);
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_COMPOSITE);
	view->DrawBitmap(sourceBitmap, sourceBounds, destBounds,
		B_FILTER_BITMAP_BILINEAR);
	view->Sync();
	destBitmap->Unlock();
	delete sourceBitmap;

	// BBitmapStream(bitmap) takes ownership of destBitmap and deletes it in
	// its own destructor - nothing else here must delete it afterwards.
	BBitmapStream destStream(destBitmap);
	status_t status = BTranslatorRoster::Default()->Translate(&destStream,
		NULL, NULL, &cookie->thumbnailData, B_WEBP_FORMAT);

	cookie->hasThumbnail = status == B_OK
		&& cookie->thumbnailData.BufferLength() > 0;
	return B_OK;
}


void
cleanup_thumbnail(void* data)
{
	delete (thumbnail_cookie*)data;
}


}	// namespace


ThumbnailAnalyser::ThumbnailAnalyser(BString name, const BVolume& volume)
	:
	FileAnalyser(name, volume)
{
}


status_t
ThumbnailAnalyser::InitCheck()
{
	return B_OK;
}


bool
ThumbnailAnalyser::_IsSupportedImage(const entry_ref& ref)
{
	BNode node(&ref);
	if (node.InitCheck() != B_OK)
		return false;

	BNodeInfo nodeInfo(&node);
	char mimeType[B_MIME_TYPE_LENGTH];
	if (nodeInfo.GetType(mimeType) != B_OK)
		return false;

	// MIME types compare case-insensitively per BMimeType's own documented
	// equality rule - HVIFTranslator's registered type and what
	// update_mime_info() actually sniffs onto a file differ in case
	// ("x-vnd.Haiku-icon" vs. "x-vnd.haiku-icon"), so a plain strcmp here
	// silently rejected every real HVIF file on disk.
	return strncasecmp(mimeType, "image/", 6) == 0
		|| strcasecmp(mimeType, kHVIFMimeType) == 0;
}


void
ThumbnailAnalyser::AnalyseEntry(const entry_ref& ref)
{
	if (!_IsSupportedImage(ref))
		return;

	BFile file(&ref, B_READ_ONLY);
	off_t size;
	if (file.InitCheck() != B_OK || file.GetSize(&size) != B_OK
		|| size > kMaxThumbnailSourceSize) {
		return;
	}

	BPath path(&ref);
	thumbnail_cookie* cookie = new(std::nothrow) thumbnail_cookie;
	if (cookie == NULL)
		return;
	cookie->path = path.Path();
	cookie->hasThumbnail = false;

	status_t status = run_with_timeout(do_create_thumbnail, cookie,
		cleanup_thumbnail, kThumbnailTimeout);
	if (status == B_TIMED_OUT) {
		// cookie now belongs to the still-running helper thread; must not
		// touch it here.
		return;
	}
	if (status != B_OK || !cookie->hasThumbnail) {
		delete cookie;
		return;
	}

	ssize_t written = file.WriteAttr(kThumbnailAttribute, B_RAW_TYPE, 0,
		cookie->thumbnailData.Buffer(), cookie->thumbnailData.BufferLength());
	if (written == (ssize_t)cookie->thumbnailData.BufferLength()) {
		// Must be after the file's own modification time, or Tracker
		// considers this thumbnail stale and tries to regenerate it itself
		// (GetThumbnailFromAttr() in Thumbnails.cpp).
		int64 created = real_time_clock();
		file.WriteAttr(kThumbnailCreationTimeAttribute, B_TIME_TYPE, 0,
			&created, sizeof(created));
	}
	delete cookie;
}


ThumbnailAddOn::ThumbnailAddOn(image_id id, const char* name)
	:
	IndexServerAddOn(id, name)
{
}


FileAnalyser*
ThumbnailAddOn::CreateFileAnalyser(const BVolume& volume)
{
	return new (std::nothrow) ThumbnailAnalyser(Name(), volume);
}


extern "C" IndexServerAddOn* (instantiate_index_server_addon)(image_id id,
	const char* name)
{
	return new (std::nothrow) ThumbnailAddOn(id, name);
}
