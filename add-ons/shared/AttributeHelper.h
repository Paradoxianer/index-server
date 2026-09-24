/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#ifndef ATTRIBUTE_HELPER_H
#define ATTRIBUTE_HELPER_H


#include <Node.h>
#include <String.h>
#include <SupportDefs.h>
#include <TypeConstants.h>
#include <fs_attr.h>


/*! Writes a string attribute only if it isn't already set to a non-empty
value - so a correction someone made by hand in Tracker (e.g. fixing a bad
embedded ID3 "Artist" tag) survives the next re-analysis instead of being
silently overwritten with the same value read back out of the file. This
was an explicit design goal discussed on the original 2006 haiku-os.org
forum thread this project grew out of ("only update empty attributes").

The tradeoff this accepts: once an attribute has any value - written by an
analyser or by hand - it no longer tracks later changes to the file's own
embedded tag. Clearing the attribute (or deleting the file's node) is what
makes it eligible to be re-derived. */
void	write_string_attr_if_empty(BNode& node, const char* name,
			const BString& value);


#endif // ATTRIBUTE_HELPER_H
