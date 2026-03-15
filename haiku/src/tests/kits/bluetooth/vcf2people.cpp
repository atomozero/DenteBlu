/*
 * vcf2people — Import a .vcf file into Haiku People files.
 * Usage: vcf2people <file.vcf> [output_directory]
 */

#include <bluetooth/PeopleWriter.h>
#include <bluetooth/VCardParser.h>

#include <File.h>
#include <ObjectList.h>

#include <stdio.h>
#include <stdlib.h>


int
main(int argc, char* argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <file.vcf> [output_directory]\n", argv[0]);
		return 1;
	}

	const char* vcfPath = argv[1];
	const char* outDir = (argc >= 3) ? argv[2] : "/boot/home/people";

	// Read entire file
	BFile file(vcfPath, B_READ_ONLY);
	if (file.InitCheck() != B_OK) {
		fprintf(stderr, "Cannot open %s: %s\n", vcfPath,
			strerror(file.InitCheck()));
		return 1;
	}

	off_t size;
	file.GetSize(&size);
	if (size <= 0) {
		fprintf(stderr, "Empty file\n");
		return 1;
	}

	uint8* data = (uint8*)malloc(size);
	if (data == NULL) {
		fprintf(stderr, "Out of memory\n");
		return 1;
	}

	ssize_t bytesRead = file.Read(data, size);
	if (bytesRead != size) {
		fprintf(stderr, "Read error\n");
		free(data);
		return 1;
	}

	// Parse vCards
	BObjectList<VCardContact, true> contacts;
	status_t result = VCardParser::Parse(data, size, &contacts);
	free(data);

	if (result != B_OK) {
		fprintf(stderr, "Parse error: %s\n", strerror(result));
		return 1;
	}

	printf("Parsed %ld contacts from %s\n", (long)contacts.CountItems(),
		vcfPath);

	// Write People files
	int32 written = PeopleWriter::WriteContacts(contacts, outDir);
	printf("Created %ld People files in %s\n", (long)written, outDir);

	return 0;
}
