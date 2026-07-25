#include "AudioTagAnalyser.h"

#include <new>
#include <string.h>

#include <MediaFile.h>
#include <Mime.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Path.h>

#include <audioproperties.h>
#include <tag.h>
#include <fileref.h>


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

	TagLib::FileRef tagFile(path.Path());
	TagLib::Tag* tag = tagFile.tag();
	if (!tag)
		return;

	TagLib::String artist = tag->artist();
	TagLib::String title = tag->title();
	TagLib::String album = tag->album();
	printf("artist: %s, title: %s, album: %s\n", artist.toCString(),
		   title.toCString(), album.toCString());

	BFile file(&ref, B_READ_ONLY);
	if (file.InitCheck() != B_OK)
		return;

	const char* cArtist = artist.toCString(true);
	file.WriteAttr("Audio:Artist", B_STRING_TYPE, 0, cArtist, strlen(cArtist));
	const char* cTitle = title.toCString(true);
	file.WriteAttr("Media:Title", B_STRING_TYPE, 0, cTitle, strlen(cTitle));
	const char* cAlbum = album.toCString(true);
	file.WriteAttr("Audio:Album", B_STRING_TYPE, 0, cAlbum, strlen(cAlbum));
/*
	BMediaFile mediaFile(&ref);
	if (mediaFile.InitCheck() != B_OK)
		return;

	BMessage metaData;
	if (mediaFile.GetMetaData(&metaData) != B_OK)
		return;

	BFile file(&ref, B_READ_ONLY);
	if (file.InitCheck() != B_OK)
		return;

	BString dataString;
	if (metaData.FindString("artist", &dataString) == B_OK)
		file.WriteAttr("Audio:Artist", B_STRING_TYPE, 0, dataString.String(),
			dataString.Length());
	if (metaData.FindString("title", &dataString) == B_OK)
		file.WriteAttr("Media:Title", B_STRING_TYPE, 0, dataString.String(),
			dataString.Length());
	if (metaData.FindString("album", &dataString) == B_OK)
		file.WriteAttr("Audio:Album", B_STRING_TYPE, 0, dataString.String(),
			dataString.Length());
	if (metaData.FindString("track", &dataString) == B_OK)
		file.WriteAttr("Audio:Track", B_STRING_TYPE, 0, dataString.String(),
			dataString.Length());*/
	
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
