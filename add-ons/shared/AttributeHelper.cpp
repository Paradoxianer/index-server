/*
 * Copyright 2026, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Matthias Lindner
 */
#include "AttributeHelper.h"


void
write_string_attr_if_empty(BNode& node, const char* name,
	const BString& value)
{
	attr_info info;
	if (node.GetAttrInfo(name, &info) == B_OK && info.size > 0)
		return;

	node.WriteAttr(name, B_STRING_TYPE, 0, value.String(), value.Length());
}
