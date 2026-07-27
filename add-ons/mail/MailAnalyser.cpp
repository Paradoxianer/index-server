/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "MailAnalyser.h"

#include <new>
#include <string.h>

#include <E-mail.h>
#include <MailMessage.h>
#include <Mime.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Path.h>
#include <String.h>

#include "CLuceneDataBase.h"
#include "IndexServerPrivate.h"
#include "RunWithTimeout.h"


// RFC822/MIME parsing is in-process, not IPC like the Media Kit analyser,
// but a deeply nested or malformed multipart message is still untrusted
// input walking a recursive parser; same defensive pattern as the other
// three analysers.
static const bigtime_t kMailReadTimeout = 5 * 1000000;

// Mail body text belongs in the same full text index FullTextAnalyser
// already maintains for this volume - it's full text like anything else,
// and CLuceneWriteDataBase's process-wide lock (see its own header
// comment) is exactly what makes two independent instances safely sharing
// that one on-disk directory. Referenced by name, not by including
// fulltext/'s header, to avoid a cross-add-on header dependency for one
// string constant.
static const char* const kFullTextDirectory = "FullTextAnalyser";


namespace {


struct mail_body_cookie {
	entry_ref	ref;
	BString		body;
	bool		hasBody;
};


status_t
do_read_mail_body(void* data)
{
	mail_body_cookie* cookie = (mail_body_cookie*)data;
	BEmailMessage message(&cookie->ref);
	if (message.InitCheck() != B_OK)
		return B_OK;

	const char* body = message.BodyText();
	if (body != NULL && body[0] != '\0') {
		cookie->body = body;
		cookie->hasBody = true;
	}
	return B_OK;
}


void
cleanup_mail_body(void* data)
{
	delete (mail_body_cookie*)data;
}


}	// namespace


MailAnalyser::MailAnalyser(BString name, const BVolume& volume)
	:
	FileAnalyser(name, volume)
{
	BPath dataBasePath = volume_index_server_directory(volume);
	dataBasePath.Append(kFullTextDirectory);
	fWriteDataBase = new CLuceneWriteDataBase(dataBasePath);
}


MailAnalyser::~MailAnalyser()
{
	delete fWriteDataBase;
}


status_t
MailAnalyser::InitCheck()
{
	return fWriteDataBase->InitCheck();
}


bool
MailAnalyser::_IsMailFile(const entry_ref& ref)
{
	BNode node(&ref);
	if (node.InitCheck() != B_OK)
		return false;

	BNodeInfo nodeInfo(&node);
	char mimeType[B_MIME_TYPE_LENGTH];
	if (nodeInfo.GetType(mimeType) != B_OK)
		return false;

	return strcmp(mimeType, B_MAIL_TYPE) == 0;
}


void
MailAnalyser::AnalyseEntry(const entry_ref& ref)
{
	if (!_IsMailFile(ref))
		return;
	printf("MailAnalyser: analysing %s\n", ref.name);

	mail_body_cookie* cookie = new(std::nothrow) mail_body_cookie;
	if (cookie == NULL)
		return;
	cookie->ref = ref;
	cookie->hasBody = false;

	status_t status = run_with_timeout(do_read_mail_body, cookie,
		cleanup_mail_body, kMailReadTimeout);
	if (status == B_TIMED_OUT) {
		// cookie now belongs to the still-running helper thread; must not
		// touch it here.
		return;
	}
	if (status != B_OK || !cookie->hasBody) {
		delete cookie;
		return;
	}

	fWriteDataBase->AddDocumentWithText(ref, cookie->body);
	delete cookie;
}


void
MailAnalyser::DeleteEntry(const entry_ref& ref)
{
	// No MIME check here: the entry no longer exists by the time a delete
	// notification arrives, so BNodeInfo::GetType() can't succeed anyway
	// (same reasoning as FullTextAnalyser::DeleteEntry). Removing a path
	// that was never indexed is a harmless no-op in CLucene.
	fWriteDataBase->RemoveDocument(ref);
}


void
MailAnalyser::LastEntry()
{
	fWriteDataBase->Commit();
}


MailAddOn::MailAddOn(image_id id, const char* name)
	:
	IndexServerAddOn(id, name)
{
}


FileAnalyser*
MailAddOn::CreateFileAnalyser(const BVolume& volume)
{
	return new (std::nothrow) MailAnalyser(Name(), volume);
}


extern "C" IndexServerAddOn* (instantiate_index_server_addon)(image_id id,
	const char* name)
{
	return new (std::nothrow) MailAddOn(id, name);
}
