#include<stdint.h>

#define MAX_FILE_SIZE 8192;
#define MAX_DIR_CAPACITY 8192;

typedef struct {
	char type[3];
	char name[11];
	uint8_t data[MAX_FILE_SIZE];
	uint32_t size;
	uint16_t creationTime;
	uint16_t creationDate;
	uint16_t lastAccessDate;
	uint16_t lastModTime;
	uint16_t lastModDate;
} mfs_file;

typedef struct {
	char name[11];
	mfs_file* contents[MAX_DIR_CAPACITY];
	uint32_t size;
	uint16_t creationTime;
	uint16_t creationDate;
	uint16_t lastAccessDate;
	uint16_t lastModTime;
	uint16_t lastModDate;
} mfs_dir;
