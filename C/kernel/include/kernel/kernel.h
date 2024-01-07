#include<stdint.h>

#define MAX_FILE_SIZE 8192
#define MAX_DIR_CAPACITY 8192

const char KERNEL_ERROR[] = "ERROR 00 - Kernel panic error\r\n";
const char CMD_ERROR[] = "ERROR 01 - Command not found\r\n";
const char ARG_COUNT_ERROR[] = "ERROR 02 - Invalid number of command-line arguments\r\n";
const char ARG_ERROR[] = "ERROR 03 - Invalid command-line arguments\r\n";
const char INODE_FULL_ERROR[] = "ERROR 04 - Inode list full\r\n";
const char DIR_EXISTS_ERROR[] = "ERROR 05 - Directory already exists\r\n";
const char FILE_EXISTS_ERROR[] = "ERROR 06 - File already exists\r\n";
const char FILE_NOT_FOUND_ERROR[] = "ERROR 07 - File not found\r\n";
const char FILE_EMPTY_ERROR[] = "ERROR 08 - File is empty\r\n";


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

typedef struct {
        uint32_t number;
        uint32_t parent;
        char type;
      //  mfs_file file;
      //  mfs_dir dir;
} Inode;


