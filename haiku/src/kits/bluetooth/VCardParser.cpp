/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * VCardParser — Parse vCard 2.1/3.0 data into contact structures.
 */

#include <bluetooth/VCardParser.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define TRACE_VCARD(fmt, ...) \
	fprintf(stderr, "VCardParser: " fmt, ##__VA_ARGS__)


static int
_HexDigit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}


/* static */
void
VCardParser::_DecodeQuotedPrintable(BString& text)
{
	BString result;
	const char* p = text.String();
	int32 len = text.Length();

	for (int32 i = 0; i < len; i++) {
		if (p[i] == '=' && i + 2 < len) {
			if (p[i + 1] == '\r' && i + 2 < len && p[i + 2] == '\n') {
				// Soft line break — skip =\r\n
				i += 2;
			} else if (p[i + 1] == '\n') {
				// Soft line break — skip =\n
				i += 1;
			} else {
				int hi = _HexDigit(p[i + 1]);
				int lo = _HexDigit(p[i + 2]);
				if (hi >= 0 && lo >= 0) {
					result += (char)((hi << 4) | lo);
					i += 2;
				} else {
					result += p[i];
				}
			}
		} else {
			result += p[i];
		}
	}

	text = result;
}


/* static */
void
VCardParser::_UnfoldLines(BString& text)
{
	// vCard line folding: a line starting with space or tab
	// is a continuation of the previous line
	text.ReplaceAll("\r\n ", "");
	text.ReplaceAll("\r\n\t", "");
	text.ReplaceAll("\n ", "");
	text.ReplaceAll("\n\t", "");
}


/* static */
void
VCardParser::_ParseProperty(const BString& name, const BString& params,
	const BString& value, VCardContact* contact)
{
	BString upperName(name);
	upperName.ToUpper();

	if (upperName == "FN") {
		contact->fullName = value;
	} else if (upperName == "N") {
		// N:lastName;firstName;middleName;prefix;suffix
		int32 semi1 = value.FindFirst(';');
		if (semi1 >= 0) {
			value.CopyInto(contact->lastName, 0, semi1);
			int32 semi2 = value.FindFirst(';', semi1 + 1);
			if (semi2 >= 0) {
				value.CopyInto(contact->firstName, semi1 + 1,
					semi2 - semi1 - 1);
			} else {
				value.CopyInto(contact->firstName, semi1 + 1,
					value.Length() - semi1 - 1);
			}
		} else {
			contact->lastName = value;
		}
	} else if (upperName == "NICKNAME") {
		contact->nickname = value;
	} else if (upperName == "TEL") {
		BString upperParams(params);
		upperParams.ToUpper();
		if (upperParams.FindFirst("CELL") >= 0
			|| upperParams.FindFirst("MOBILE") >= 0) {
			contact->mobilePhone = value;
		} else if (upperParams.FindFirst("HOME") >= 0) {
			contact->homePhone = value;
		} else if (upperParams.FindFirst("WORK") >= 0) {
			contact->workPhone = value;
		} else if (upperParams.FindFirst("FAX") >= 0) {
			contact->fax = value;
		} else {
			// Default to mobile
			contact->mobilePhone = value;
		}
	} else if (upperName == "EMAIL") {
		contact->email = value;
	} else if (upperName == "ADR") {
		// ADR:PO;ext;street;city;state;zip;country
		const char* p = value.String();
		int field = 0;
		BString current;
		for (int32 i = 0; i <= value.Length(); i++) {
			if (i == value.Length() || p[i] == ';') {
				switch (field) {
					case 2: contact->address = current; break;
					case 3: contact->city = current; break;
					case 4: contact->state = current; break;
					case 5: contact->zip = current; break;
					case 6: contact->country = current; break;
				}
				current = "";
				field++;
			} else {
				current += p[i];
			}
		}
	} else if (upperName == "ORG") {
		// ORG may have sub-fields separated by ;
		int32 semi = value.FindFirst(';');
		if (semi >= 0)
			value.CopyInto(contact->company, 0, semi);
		else
			contact->company = value;
	} else if (upperName == "NOTE") {
		contact->note = value;
	} else if (upperName == "URL") {
		contact->url = value;
	} else if (upperName == "CATEGORIES") {
		contact->group = value;
	}
}


/* static */
status_t
VCardParser::_ParseSingleVCard(const BString& block, VCardContact* contact)
{
	int32 len = block.Length();
	int32 pos = 0;

	while (pos < len) {
		// Find end of line
		int32 eol = block.FindFirst('\n', pos);
		if (eol < 0)
			eol = len;

		BString line;
		block.CopyInto(line, pos, eol - pos);
		line.RemoveAll("\r");
		pos = eol + 1;

		// Skip empty lines and BEGIN/END
		if (line.Length() == 0)
			continue;
		BString upperLine(line);
		upperLine.ToUpper();
		if (upperLine.StartsWith("BEGIN:") || upperLine.StartsWith("END:")
			|| upperLine.StartsWith("VERSION:")) {
			continue;
		}

		// Split property:  NAME;params:value
		int32 colonPos = line.FindFirst(':');
		if (colonPos < 0)
			continue;

		BString nameAndParams;
		BString value;
		line.CopyInto(nameAndParams, 0, colonPos);
		line.CopyInto(value, colonPos + 1, line.Length() - colonPos - 1);

		BString propName;
		BString params;
		int32 semiPos = nameAndParams.FindFirst(';');
		if (semiPos >= 0) {
			nameAndParams.CopyInto(propName, 0, semiPos);
			nameAndParams.CopyInto(params, semiPos + 1,
				nameAndParams.Length() - semiPos - 1);
		} else {
			propName = nameAndParams;
		}

		// Check for QUOTED-PRINTABLE encoding
		BString upperParams(params);
		upperParams.ToUpper();
		if (upperParams.FindFirst("QUOTED-PRINTABLE") >= 0)
			_DecodeQuotedPrintable(value);

		_ParseProperty(propName, params, value, contact);
	}

	// If no FN, synthesize from N
	if (contact->fullName.Length() == 0) {
		if (contact->firstName.Length() > 0
			&& contact->lastName.Length() > 0) {
			contact->fullName = contact->firstName;
			contact->fullName += " ";
			contact->fullName += contact->lastName;
		} else if (contact->lastName.Length() > 0) {
			contact->fullName = contact->lastName;
		} else if (contact->firstName.Length() > 0) {
			contact->fullName = contact->firstName;
		}
	}

	return B_OK;
}


/* static */
status_t
VCardParser::Parse(const uint8* data, size_t length,
	BObjectList<VCardContact, true>* outContacts)
{
	if (data == NULL || length == 0 || outContacts == NULL)
		return B_BAD_VALUE;

	BString text((const char*)data, length);
	_UnfoldLines(text);

	int32 pos = 0;
	int32 total = text.Length();
	int count = 0;

	while (pos < total) {
		// Find BEGIN:VCARD
		int32 begin = text.IFindFirst("BEGIN:VCARD", pos);
		if (begin < 0)
			break;

		// Find END:VCARD
		int32 end = text.IFindFirst("END:VCARD", begin);
		if (end < 0)
			break;

		int32 blockEnd = end + 9; // length of "END:VCARD"
		BString block;
		text.CopyInto(block, begin, blockEnd - begin);

		VCardContact* contact = new(std::nothrow) VCardContact;
		if (contact == NULL)
			return B_NO_MEMORY;

		status_t result = _ParseSingleVCard(block, contact);
		if (result == B_OK && contact->fullName.Length() > 0) {
			outContacts->AddItem(contact);
			count++;
		} else {
			delete contact;
		}

		pos = blockEnd;
	}

	TRACE_VCARD("Parsed %d contacts\n", count);
	return B_OK;
}
