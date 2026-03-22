#include<stdint.h>
#include<filesystem/mfs_file.h>
#ifndef MFS_H
#define MFS_H
typedef struct {
	uint32_t number;
	uint32_t parent;
	char type;
	char name[32];
	mfs_file file;
	mfs_dir dir;
} Inode;

extern Inode inodeList[1024];
extern size_t inodeCount;
extern uint32_t currentInode;

void listContents();

void createDirectory(const char* name);

void createFile(const char* name);

char *uint32_to_str(uint32_t i);

#endif
