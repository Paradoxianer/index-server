/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "ThumbnailAnalyser.h"

#include <new>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <File.h>
#include <FindDirectory.h>
#include <Mime.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Path.h>

#include "RunThumbnailHelper.h"


// A malformed or pathological image must not stall the whole VolumeWorker
// thread over decoding/scaling it - now bounds run_thumbnail_helper()
// instead of an in-process run_with_timeout() call (see
// RunThumbnailHelper.h for why decoding/composing moved into its own
// isolated helper process).
const bigtime_t kThumbnailTimeout = 15 * 1000000;

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

	node_ref nodeRef;
	if (file.GetNodeRef(&nodeRef) != B_OK)
		return;

	BPath path(&ref);

	// Scratch space for the isolated helper's output - not this add-on's
	// own persistent data (it has none; the result ends up as an
	// attribute on the source file itself), so the system temp directory
	// is the right place, not fDataBasePath-style storage the way
	// FullTextAnalyser uses for its own temp files. Keyed by node_ref for
	// the same reason FullTextAnalyser's temp names are: more than one
	// file can be in flight across volumes' worker threads at once, so no
	// two may share a path.
	BPath tempPath;
	if (find_directory(B_SYSTEM_TEMP_DIRECTORY, &tempPath) != B_OK)
		return;
	BString tempName;
	tempName.SetToFormat("index_server_thumbnail_%" B_PRId64,
		(int64)nodeRef.node);
	tempPath.Append(tempName.String());

	// ThumbnailHelper.cpp only ever creates tempPath itself, by rename()
	// on success - a timeout or any other failure leaves nothing there,
	// so there's nothing to clean up in either failure case here.
	status_t status = run_thumbnail_helper(path.Path(), tempPath.Path(),
		kThumbnailTimeout);
	if (status != B_OK)
		return;

	BFile thumbnailFile(tempPath.Path(), B_READ_ONLY);
	off_t thumbnailSize;
	if (thumbnailFile.InitCheck() != B_OK
		|| thumbnailFile.GetSize(&thumbnailSize) != B_OK
		|| thumbnailSize <= 0) {
		remove(tempPath.Path());
		return;
	}

	uint8* buffer = new(std::nothrow) uint8[thumbnailSize];
	if (buffer == NULL
		|| thumbnailFile.Read(buffer, thumbnailSize) != thumbnailSize) {
		delete[] buffer;
		remove(tempPath.Path());
		return;
	}
	remove(tempPath.Path());

	ssize_t written = file.WriteAttr(kThumbnailAttribute, B_RAW_TYPE, 0,
		buffer, thumbnailSize);
	if (written == (ssize_t)thumbnailSize) {
		// Must be after the file's own modification time, or Tracker
		// considers this thumbnail stale and tries to regenerate it itself
		// (GetThumbnailFromAttr() in Thumbnails.cpp).
		int64 created = real_time_clock();
		file.WriteAttr(kThumbnailCreationTimeAttribute, B_TIME_TYPE, 0,
			&created, sizeof(created));
	}
	delete[] buffer;
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
