// MOSS KERNEL - Filesystem Commands
// (C) Miro Haapalainen, 2024

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <kernel/kernel.h>

extern Inode inodeList[1024];
extern char fileNames[1024][32];
extern mfs_file files[1024];
extern mfs_dir dirs[1024];
extern size_t inodeCount;
extern uint32_t currentInode;

int isFile(char* name) {
	for (int i=0;i<1024;i++) {
		if (inodeList[i].parent == currentInode && memcmp(fileNames[i], name, strlen(name)) == 0) {
			return i;
		}
	}
	return -1;
}

void ls_cmd(void) {
	printf(".\r\n..\r\n");
	for (int i=0;i<1024;i++) {
		if (inodeList[i].parent == currentInode && (inodeList[i].type == 'd' || inodeList[i].type == 'f') && inodeList[i].number != currentInode) {
			if (inodeList[i].type == 'd') {
				printf("[%s]   ", fileNames[inodeList[i].number]);
				printf("%d\0", dirs[i].size);
				printf(" bytes\r\n");
			} else {
				printf("%s ", fileNames[inodeList[i].number]);
				printf("%s   ", files[i].type);
				printf("%d\0", files[i].size);
				printf(" bytes\r\n");
			}
		}
	}
}

void mkdir_cmd(void) {
	if (argc != 2) {
		printf(ARG_COUNT_ERROR);
		return; 
	}
        if (inodeCount == 1024) {
                printf(INODE_FULL_ERROR);
                return;
        }
        for (int i=0;i<1024;i++) {
                if (memcmp(&fileNames[inodeList[i].number], argv[1], strlen(argv[1])) == 0 && 
				inodeList[i].parent == currentInode) {
                        printf(DIR_EXISTS_ERROR);
                        return;
                }
        }
        Inode i1;
	i1.type = 'd';
        i1.number = inodeCount;
	memcpy(&fileNames[inodeCount], argv[1], strlen(argv[1]));
        i1.parent = currentInode;
	mfs_dir d1;
	d1.size = 0;
	dirs[inodeCount] = d1;
        inodeList[inodeCount++] = i1;
}

void touch_cmd(void) {
	if (argc != 2) {
		printf(ARG_COUNT_ERROR);
		return;
	}
	if (inodeCount == 1024) {
		printf(INODE_FULL_ERROR);
	}
	for (int i=0;i<1024;i++) {
		if (inodeList[i].parent == currentInode && 
		memcmp(fileNames[inodeList[i].number], argv[1], strlen(argv[1])) == 0) {
			printf(FILE_EXISTS_ERROR);
			return;
		}
	}
	Inode i1;
	i1.number = inodeCount;
	i1.type = 'f';
	memcpy(&fileNames[inodeCount], argv[1], strlen(argv[1]));
	i1.parent = currentInode;
	mfs_file f1;
	files[inodeCount] = f1;
	files[inodeCount].size = 0;
	memcpy(&files[inodeCount].type, "MDF", strlen("MDF"));
	inodeList[inodeCount++] = i1;
}

void cd_cmd(void) {
	if (argc == 1) {
		currentInode = 0;
	} else if (argc == 2) {
		if (memcmp(argv[1], "..", strlen("..")) == 0) {
			currentInode = inodeList[currentInode].parent;
		} else {
			for (int i = 0; i < 1024; i++) {
				if (inodeList[i].parent == currentInode && 
				memcmp(fileNames[inodeList[i].number], argv[1], strlen(argv[1]))== 0) {
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

void mv_cmd(void) {
	/* TODO */
}

void rm_cmd(void) {
	if (argc != 2) {
		printf(ARG_COUNT_ERROR);
		return;
	}
	int id = isFile(argv[1]);
	if (id == -1) {
		printf(FILE_NOT_FOUND_ERROR);
		return;
	}
	if (inodeList[id].type == 'd') {
		printf("Invalid argument - Use rmdir to remove directories\r\n");
	}
	memset(&inodeList[id], 0, sizeof(Inode));
	inodeCount--;
}

void cat_cmd(void) {
	if (argc == 1) {
		/* TODO */
	} else if (argc == 2) {
		int id = isFile(argv[1]);
		if (id != -1) {
			int len = strlen((char *)files[id].data);
			if (len == 0) {
				printf(FILE_EMPTY_ERROR);
				return;
			}
			for (int i=0;i<len;i++) {
				putchar(files[id].data[i]);
			}
			printf("\r\n");
		} else {
			printf(FILE_NOT_FOUND_ERROR);
			return;
		}
	} else {
		printf(ARG_COUNT_ERROR);
		return;
	}
}

void rmdir_cmd(void) {
	/* TODO */
}
