/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * VCardParser — Parse vCard 2.1/3.0 data into contact structures.
 */
#ifndef _VCARD_PARSER_H_
#define _VCARD_PARSER_H_

#include <ObjectList.h>
#include <String.h>


struct VCardContact {
	BString		fullName;
	BString		lastName;
	BString		firstName;
	BString		nickname;
	BString		company;
	BString		homePhone;
	BString		workPhone;
	BString		mobilePhone;
	BString		fax;
	BString		email;
	BString		address;
	BString		city;
	BString		state;
	BString		zip;
	BString		country;
	BString		url;
	BString		note;
	BString		group;
};


class VCardParser {
public:
	static status_t				Parse(const uint8* data, size_t length,
									BObjectList<VCardContact, true>* outContacts);

private:
	static status_t				_ParseSingleVCard(const BString& block,
									VCardContact* contact);
	static void					_UnfoldLines(BString& text);
	static void					_ParseProperty(const BString& name,
									const BString& params,
									const BString& value,
									VCardContact* contact);
	static void					_DecodeQuotedPrintable(BString& text);
};


#endif /* _VCARD_PARSER_H_ */
