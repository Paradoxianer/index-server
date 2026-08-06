/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "MediaKitAnalyser.h"

#include <new>
#include <string.h>
#include <strings.h>

#include <File.h>
#include <MediaFile.h>
#include <MediaTrack.h>
#include <Mime.h>
#include <Node.h>
#include <NodeInfo.h>
#include <String.h>

#include "RunWithTimeout.h"


// BMediaFile/BMediaTrack talk to media_server over IPC to negotiate a
// decoder; a wedged media_server or a malformed file that confuses a codec
// add-on must not be able to stall the whole VolumeWorker/CatchUpAnalyser
// thread, same reasoning as the other two analysers.
static const bigtime_t kMediaInfoTimeout = 5 * 1000000;


namespace {


struct media_info_cookie {
	entry_ref	ref;
	bool		isVideo;
	bool		isAudio;
	BString		codecName;
	bigtime_t	duration;
	uint32		width;
	uint32		height;
	bool		hasData;
};


status_t
do_read_media_info(void* data)
{
	media_info_cookie* cookie = (media_info_cookie*)data;

	BMediaFile mediaFile(&cookie->ref);
	if (mediaFile.InitCheck() != B_OK || mediaFile.CountTracks() == 0)
		return B_OK;

	BMediaTrack* track = mediaFile.TrackAt(0);
	if (track == NULL)
		return B_OK;

	media_format format;
	if (track->EncodedFormat(&format) == B_OK) {
		cookie->isVideo = format.IsVideo();
		cookie->isAudio = format.IsAudio();
		if (cookie->isVideo) {
			cookie->width = format.Width();
			cookie->height = format.Height();
		}
	}

	media_codec_info codecInfo;
	if (track->GetCodecInfo(&codecInfo) == B_OK)
		cookie->codecName = codecInfo.pretty_name;

	cookie->duration = track->Duration();
	cookie->hasData = true;

	mediaFile.ReleaseTrack(track);
	return B_OK;
}


void
cleanup_media_info(void* data)
{
	delete (media_info_cookie*)data;
}


}	// namespace


MediaKitAnalyser::MediaKitAnalyser(BString name, const BVolume& volume)
	:
	FileAnalyser(name, volume)
{
}


status_t
MediaKitAnalyser::InitCheck()
{
	return B_OK;
}


bool
MediaKitAnalyser::_IsSupportedMedia(const entry_ref& ref)
{
	BNode node(&ref);
	if (node.InitCheck() != B_OK)
		return false;

	BNodeInfo nodeInfo(&node);
	char mimeType[B_MIME_TYPE_LENGTH];
	if (nodeInfo.GetType(mimeType) != B_OK)
		return false;

	// MIME types compare case-insensitively per BMimeType's own documented
	// equality rule (see #49).
	return strncasecmp(mimeType, "audio/", 6) == 0
		|| strncasecmp(mimeType, "video/", 6) == 0;
}


void
MediaKitAnalyser::AnalyseEntry(const entry_ref& ref)
{
	if (!_IsSupportedMedia(ref))
		return;

	media_info_cookie* cookie = new(std::nothrow) media_info_cookie;
	if (cookie == NULL)
		return;
	cookie->ref = ref;
	cookie->isVideo = false;
	cookie->isAudio = false;
	cookie->hasData = false;

	status_t status = run_with_timeout(do_read_media_info, cookie,
		cleanup_media_info, kMediaInfoTimeout);
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
		if (cookie->codecName.Length() > 0) {
			file.WriteAttr("Media:Codec", B_STRING_TYPE, 0,
				cookie->codecName.String(), cookie->codecName.Length());
		}
		if (cookie->duration > 0) {
			// "Media:Length" (raw microseconds, same as BMediaTrack::
			// Duration() - no unit conversion) is the attribute Haiku's own
			// audio.super/video.super MIME definitions and MediaPlayer
			// already use for this, not something to invent a new name for.
			int64 length = cookie->duration;
			file.WriteAttr("Media:Length", B_INT64_TYPE, 0, &length,
				sizeof(length));
		}
		if (cookie->isVideo && cookie->width > 0 && cookie->height > 0) {
			// Same attribute names/type Tracker's own thumbnailer already
			// uses for images (see the EXIF analyser's live test), so
			// anything that already shows a "Width"/"Height" column picks
			// these up for video too without extra configuration.
			int32 width = (int32)cookie->width;
			int32 height = (int32)cookie->height;
			file.WriteAttr("Media:Width", B_INT32_TYPE, 0, &width,
				sizeof(width));
			file.WriteAttr("Media:Height", B_INT32_TYPE, 0, &height,
				sizeof(height));
		}
	}
	delete cookie;
}


MediaKitAddOn::MediaKitAddOn(image_id id, const char* name)
	:
	IndexServerAddOn(id, name)
{
}


FileAnalyser*
MediaKitAddOn::CreateFileAnalyser(const BVolume& volume)
{
	return new (std::nothrow) MediaKitAnalyser(Name(), volume);
}


extern "C" IndexServerAddOn* (instantiate_index_server_addon)(image_id id,
	const char* name)
{
	return new (std::nothrow) MediaKitAddOn(id, name);
}
