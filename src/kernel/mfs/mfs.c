#include<stdint.h>
#include<stdio.h>
#include<string.h>
#include<filesystem/mfs.h>
#include<filesystem/mfs_file.h>
#include<kernel/kernel.h>

const char INODE_FULL_ERROR = "ERROR 04 - Inode list full\r\n";
const char DIR_EXISTS_ERROR = "ERROR 05 - Directory already exists\r\n";


void changeDirectory(const char *name) {
	if (argc == 1) {
		currentInode = 0;
	} else if (argc == 2) {
		if (memcmp(argv[1], "..", strlen("..")) == 0) {
			currentInode = inodeList[currentInode].parent;
		} else {
			for (int i = 0; i < 1024; i++) {
				if (inodeList[i].parent == currentInode && memcmp(fileNames[inodeList[i].number], argv[1], strlen(argv[1]))== 0) {
					if (inodeList[i].type != 'd') {
						printf(ARG_ERROR);
						return;
					}
					currentInode = i;
				}
			}	
		} 
	} else {
		printf(ARG_COUNT_ERROR);
	}
}

void listContents() {
	printf(".\r\n..\r\n");
	for (int i=0;i<1024;i++) {
		if (inodeList[i].parent == currentInode && (inodeList[i].type == 'd' || inodeList[i].type == 'f') && inodeList[i].number != currentInode) {
			printf("%s\r\n", inodeList[i].name);
		}
	}
}

void createDirectory(const char *name) {
	char str1[32];
	char str2[32];
	memset(str1, 0, sizeof(str1));
	memset(str2, 0, sizeof(str2));
	str1[0] = '.';
	str2[0] = '.';
	str2[1] = '.';
	if (inodeCount == 1024) {
		printf(INODE_FULL_ERROR);
		return;
	}
	for (int i=0;i<1024;i++) {
		if (strcmp(inodeList[i].name, name) == 0 && inodeList[i].parent == currentInode) {
			printf(DIR_EXISTS_ERROR);
			return;
		}
	}
	Inode i1;
	memset(&i1, 0, sizeof(i1));
	strncpy(i1.name, name, 32);
	i1.type = 'd';
	i1.number = inodeCount;
	i1.parent = currentInode;
	i1.dir.name = name;
	memset(i1.dir.contents, 0, sizeof(i1.dir.contents));
	i1.dir.size = 0;
	inodeList[inodeCount] = i1;
	memset(mfs_dir.contents, 0, sizeof(mfs_dir.contents));
	

}
