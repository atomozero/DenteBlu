/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * PeopleWriter — Write VCardContact data as Haiku People files.
 */
#ifndef _PEOPLE_WRITER_H_
#define _PEOPLE_WRITER_H_

#include <ObjectList.h>
#include <String.h>
#include <SupportDefs.h>

struct VCardContact;


class PeopleWriter {
public:
	static int32				WriteContacts(
									const BObjectList<VCardContact, true>& contacts,
									const char* directory = NULL);
	static status_t				WriteContact(const VCardContact& contact,
									const char* directory = NULL);

private:
	static BString				_MakeFilename(const char* fullName,
									const char* directory);
	static status_t				_WriteAttrString(int fd,
									const char* attr,
									const BString& value);
};


#endif /* _PEOPLE_WRITER_H_ */
