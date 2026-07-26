#include "AudioTagAnalyser.h"

#include <new>
#include <string.h>

#include <File.h>
#include <Mime.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Path.h>
#include <String.h>

#include <tag.h>
#include <fileref.h>

#include "RunWithTimeout.h"


// A hung or pathological file must not stall the whole VolumeWorker thread
// (it processes every entry of a volume serially), same reasoning as
// FullTextAnalyser's translator timeout.
static const bigtime_t kTagReadTimeout = 5 * 1000000;


namespace {


// Owns the read-out tag fields itself rather than pointing at the caller's
// stack locals: on timeout, ownership passes to the still-running helper
// thread (see RunWithTimeout.h), which may still be inside TagLib.
struct tag_read_cookie {
	BString	path;
	BString	artist;
	BString	title;
	BString	album;
	bool	hasTag;
};


status_t
do_read_tags(void* data)
{
	tag_read_cookie* cookie = (tag_read_cookie*)data;
	TagLib::FileRef tagFile(cookie->path.String());
	TagLib::Tag* tag = tagFile.tag();
	cookie->hasTag = (tag != NULL);
	if (tag != NULL) {
		cookie->artist = tag->artist().toCString(true);
		cookie->title = tag->title().toCString(true);
		cookie->album = tag->album().toCString(true);
	}
	return B_OK;
}


void
cleanup_read_tags(void* data)
{
	delete (tag_read_cookie*)data;
}


}	// namespace


AudioTagAnalyser::AudioTagAnalyser(BString name, const BVolume& volume)
	:
	FileAnalyser(name, volume)
{

}


status_t
AudioTagAnalyser::InitCheck()
{
	return B_OK;
}



bool
AudioTagAnalyser::_IsAudioFile(const entry_ref& ref)
{
	BNode node(&ref);
	if (node.InitCheck() != B_OK)
		return false;

	BNodeInfo nodeInfo(&node);
	char mimeType[B_MIME_TYPE_LENGTH];
	if (nodeInfo.GetType(mimeType) != B_OK)
		return false;

	return strncmp(mimeType, "audio/", 6) == 0;
}


void
AudioTagAnalyser::AnalyseEntry(const entry_ref& ref)
{
	// TagLib crashes on some non-audio input, having no reason to expect
	// anything else; only hand it files the system already recognizes as
	// audio.
	if (!_IsAudioFile(ref))
		return;

	BPath path(&ref);

	tag_read_cookie* cookie = new(std::nothrow) tag_read_cookie;
	if (cookie == NULL)
		return;
	cookie->path = path.Path();
	cookie->hasTag = false;

	status_t status = run_with_timeout(do_read_tags, cookie,
		cleanup_read_tags, kTagReadTimeout);
	if (status == B_TIMED_OUT) {
		// cookie now belongs to the still-running helper thread; must not
		// touch it here.
		return;
	}
	if (status != B_OK || !cookie->hasTag) {
		delete cookie;
		return;
	}

	BFile file(&ref, B_READ_ONLY);
	if (file.InitCheck() == B_OK) {
		file.WriteAttr("Audio:Artist", B_STRING_TYPE, 0,
			cookie->artist.String(), cookie->artist.Length());
		file.WriteAttr("Media:Title", B_STRING_TYPE, 0,
			cookie->title.String(), cookie->title.Length());
		file.WriteAttr("Audio:Album", B_STRING_TYPE, 0,
			cookie->album.String(), cookie->album.Length());
	}
	delete cookie;
}


AudioTagAddOn::AudioTagAddOn(image_id id, const char* name)
	:
	IndexServerAddOn(id, name)
{
	
}


FileAnalyser*
AudioTagAddOn::CreateFileAnalyser(const BVolume& volume)
{
	return new (std::nothrow)AudioTagAnalyser(Name(), volume);
}


extern "C" IndexServerAddOn* (instantiate_index_server_addon)(image_id id,
	const char* name)
{
	return new (std::nothrow)AudioTagAddOn(id, name);
}
