/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * PeopleWriter — Write VCardContact data as Haiku People files.
 */

#include <bluetooth/PeopleWriter.h>
#include <bluetooth/VCardParser.h>

#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <NodeInfo.h>
#include <Path.h>
#include <String.h>
#include <fs_attr.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>


#define TRACE_PEOPLE(fmt, ...) \
	fprintf(stderr, "PeopleWriter: " fmt, ##__VA_ARGS__)

static const char* kDefaultDir = "/boot/home/people";


static void
_WriteOne(int fd, const char* attr, const BString& value)
{
	if (value.Length() > 0) {
		fs_write_attr(fd, attr, B_STRING_TYPE, 0,
			value.String(), value.Length() + 1);
	}
}


/* static */
BString
PeopleWriter::_MakeFilename(const char* fullName, const char* directory)
{
	BString name(fullName);
	name.ReplaceAll("/", "_");
	name.ReplaceAll(":", "_");

	const char* dir = (directory != NULL) ? directory : kDefaultDir;

	BString path;
	path.SetToFormat("%s/%s", dir, name.String());

	// If file exists, append (2), (3), etc.
	struct stat st;
	if (stat(path.String(), &st) != 0)
		return path;

	for (int i = 2; i < 1000; i++) {
		BString numbered;
		numbered.SetToFormat("%s/%s (%d)", dir, name.String(), i);
		if (stat(numbered.String(), &st) != 0)
			return numbered;
	}

	return path;
}


/* static */
status_t
PeopleWriter::WriteContact(const VCardContact& contact,
	const char* directory)
{
	if (contact.fullName.Length() == 0)
		return B_BAD_VALUE;

	const char* dir = (directory != NULL) ? directory : kDefaultDir;

	// Ensure directory exists
	mkdir(dir, 0755);

	BString filePath = _MakeFilename(contact.fullName.String(), dir);

	BFile file;
	status_t result = file.SetTo(filePath.String(),
		B_CREATE_FILE | B_WRITE_ONLY | B_ERASE_FILE);
	if (result != B_OK) {
		TRACE_PEOPLE("Cannot create %s: %s\n", filePath.String(),
			strerror(result));
		return result;
	}

	// Set MIME type to People file
	BNodeInfo info(&file);
	info.SetType("application/x-person");

	int fd = file.Dup();
	if (fd < 0)
		return B_ERROR;

	_WriteOne(fd, "META:name", contact.fullName);
	_WriteOne(fd, "META:nickname", contact.nickname);
	_WriteOne(fd, "META:company", contact.company);
	_WriteOne(fd, "META:hphone", contact.homePhone);
	_WriteOne(fd, "META:mphone", contact.mobilePhone);
	_WriteOne(fd, "META:wphone", contact.workPhone);
	_WriteOne(fd, "META:fax", contact.fax);
	_WriteOne(fd, "META:email", contact.email);
	_WriteOne(fd, "META:address", contact.address);
	_WriteOne(fd, "META:city", contact.city);
	_WriteOne(fd, "META:state", contact.state);
	_WriteOne(fd, "META:zip", contact.zip);
	_WriteOne(fd, "META:country", contact.country);
	_WriteOne(fd, "META:url", contact.url);
	_WriteOne(fd, "META:note", contact.note);
	_WriteOne(fd, "META:group", contact.group);

	close(fd);

	TRACE_PEOPLE("Created %s\n", filePath.String());
	return B_OK;
}


/* static */
int32
PeopleWriter::WriteContacts(const BObjectList<VCardContact, true>& contacts,
	const char* directory)
{
	int32 count = 0;
	for (int32 i = 0; i < contacts.CountItems(); i++) {
		VCardContact* contact = contacts.ItemAt(i);
		if (contact != NULL && WriteContact(*contact, directory) == B_OK)
			count++;
	}

	TRACE_PEOPLE("Wrote %ld People files\n", (long)count);
	return count;
}
