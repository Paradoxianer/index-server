/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */

// Standalone helper spawned by ThumbnailAnalyser (via
// RunThumbnailHelper.h, add-ons/shared/) to decode an image, compose it
// into a fixed-size WebP thumbnail, and write the result out - in a
// throwaway team of its own. Same reasoning as
// translate-helper/TranslateHelper.cpp: BTranslationUtils::GetBitmap()
// and BTranslatorRoster::Translate() both run whatever third-party
// translator add-on claims an image's format against untrusted content,
// and a crash there must not take index_server down with it.
//
// Unlike TranslateHelper, this one genuinely needs a live app_server
// connection: BBitmap/BView composition hangs indefinitely without one
// (confirmed empirically - BTranslationUtils::GetBitmap() under
// BServer(initGUI=false) simply never returns, no error, no timeout of
// its own). So this is a plain BApplication rather than headless - it
// just never calls Run() or shows a window. Constructing it is what
// establishes the app_server link; nothing here depends on a message
// loop actually running.
//
// disable_debugger() below is why a crash here doesn't also pop up
// Haiku's own crash-report alert on the user's desktop - see
// TranslateHelper.cpp's identical comment for the full reasoning.
//
// usage:
//   IndexServerThumbnailHelper <sourcePath> <destPath>
//
// Exit code 0 on success, nonzero on any handled failure (unsupported
// format, decode error, encode error). destPath is only ever created by
// rename() from a sibling ".part" file on success - same reasoning as
// TranslateHelper.cpp: a crash mid-write must never leave a partial
// thumbnail sitting at the path the parent expects a finished one at.

#include <stdio.h>
#include <stdlib.h>

#include <Application.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <DataIO.h>
#include <File.h>
#include <OS.h>
#include <String.h>
#include <TranslationUtils.h>
#include <TranslatorFormats.h>
#include <TranslatorRoster.h>
#include <View.h>


// Larger than Tracker's own icon sizes so a future viewer has some room,
// small enough to stay cheap to generate and store per file. Must stay
// 128 - Tracker's own GetThumbnailFromAttr() (Thumbnails.cpp) only
// imports a stored thumbnail without a rescale when it's exactly
// B_XXL_ICON (128) square; anything else silently fails that import.
const int32 kThumbnailSize = 128;


static int
do_create_thumbnail(const char* sourcePath, const char* destPath)
{
	BFile file(sourcePath, B_READ_ONLY);
	if (file.InitCheck() != B_OK)
		return 1;

	// BTranslationUtils::GetBitmap() is the exact same
	// Translate(B_TRANSLATOR_BITMAP) + DetachBitmap() pair
	// ThumbnailAnalyser.cpp used to do by hand before this moved out here.
	BBitmap* sourceBitmap = BTranslationUtils::GetBitmap(&file);
	if (sourceBitmap == NULL)
		return 1;

	BRect sourceBounds = sourceBitmap->Bounds();
	float sourceWidth = sourceBounds.Width() + 1;
	float sourceHeight = sourceBounds.Height() + 1;
	if (sourceWidth <= 0 || sourceHeight <= 0) {
		delete sourceBitmap;
		return 1;
	}

	// A canvas sized to the source's own aspect ratio (e.g. 128x64 for a
	// wide image) would silently fail Tracker's own import - see this
	// file's own top comment on kThumbnailSize. Always emit a fixed
	// square canvas, letterboxing the scaled content centered within it,
	// exactly like Tracker's own ScaleBitmap()/ThumbBounds() do, so the
	// two stay bit-compatible.
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
		return 1;
	}

	BView* view = new(std::nothrow) BView(canvasBounds, "thumb",
		B_FOLLOW_NONE, B_WILL_DRAW);
	if (view == NULL) {
		delete sourceBitmap;
		delete destBitmap;
		return 1;
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

	// BBitmapStream(bitmap) takes ownership of destBitmap and deletes it
	// in its own destructor - nothing else here must delete it afterwards.
	BBitmapStream destStream(destBitmap);
	BMallocIO encoded;
	status_t status = BTranslatorRoster::Default()->Translate(&destStream,
		NULL, NULL, &encoded, B_WEBP_FORMAT);
	if (status != B_OK || encoded.BufferLength() == 0)
		return 1;

	BString tempPath(destPath);
	tempPath << ".part";
	{
		// Scoped so the BFile destructor flushes and closes destination
		// before the rename() below.
		BFile destination(tempPath.String(),
			B_READ_WRITE | B_CREATE_FILE | B_ERASE_FILE);
		if (destination.InitCheck() != B_OK)
			return 1;

		ssize_t written = destination.Write(encoded.Buffer(),
			encoded.BufferLength());
		if (written != (ssize_t)encoded.BufferLength()) {
			remove(tempPath.String());
			return 1;
		}
	}

	if (rename(tempPath.String(), destPath) != 0) {
		remove(tempPath.String());
		return 1;
	}
	return 0;
}


int
main(int argc, char** argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <sourcePath> <destPath>\n", argv[0]);
		return 2;
	}

	// A plain BApplication, not BServer(initGUI=false) - see this file's
	// own top comment for why a real app_server connection is required
	// here specifically. Never calls Run(); constructing it is enough to
	// establish the connection BBitmap/BView composition below needs.
	BApplication app(
		"application/x-vnd.Haiku-index_server_thumbnail_helper");

	// Never let a fault below trigger Haiku's own crash-report dialog -
	// see this file's own top comment for why.
	disable_debugger(1);

	return do_create_thumbnail(argv[1], argv[2]);
}
