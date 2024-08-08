#include<stdint.h>

#define MAX_FILE_SIZE 8192
#define MAX_DIR_CAPACITY 8192
#define NUM_COMMANDS 21
#define MAX_ARGS 10

const char KERNEL_PANIC_ERROR[] = "ERROR 00 - Kernel panic error\r\n";
const char CMD_ERROR[] = "ERROR 01 - Command not found\r\n";
const char ARG_COUNT_ERROR[] = "ERROR 02 - Invalid number of command-line arguments\r\n";
const char ARG_ERROR[] = "ERROR 03 - Invalid command-line arguments\r\n";
const char INODE_FULL_ERROR[] = "ERROR 04 - Inode list full\r\n";
const char DIR_EXISTS_ERROR[] = "ERROR 05 - Directory already exists\r\n";
const char FILE_EXISTS_ERROR[] = "ERROR 06 - File already exists\r\n";
const char FILE_NOT_FOUND_ERROR[] = "ERROR 07 - File not found\r\n";
const char FILE_EMPTY_ERROR[] = "ERROR 08 - File is empty\r\n";
const char DIR_NOT_FOUND_ERROR[] = "ERROR 09 - Directory not found\r\n";
const char KERNEL_ERROR_FIN[] = "VIRHE 00 - Kerneli-paniikki\r\n";
const char CMD_ERROR_FIN[] = {'V','I','R','H','E',' ','0','1',' ','-',' ','k','o','m','e','n','t','o','a',' ','e','i',' ','l',148,'y','t','y','n','y','t','\r','\n'};
const char ARG_COUNT_ERROR_FIN[] = {'V','I','R','H','E',' ','0','2',' ','-',' ','V',132,132,'r',132,' ','m',132,132,' ','r',132,' ','a','r','g','u','m','e','n','t','t','e','j','a','\r','\n'};

const char version[] = "Beta 1.0";
const char welcome1_eng[] = "------------------------------\r\nMOSS Kernel - Version ";
const char welcome1_fin[] = "------------------------------\r\nMOSS-kerneli - Versio ";
const char welcome2[] = "\r\n------------------------------\r\n";
const char prompt[] = ">";
char cmd_str[256];
char *cmd_str_ptr = cmd_str;
uint8_t input_char = 0;
uint8_t input_length;
int argc = 0;
char *argv[MAX_ARGS] = {0};
int language = 0;
const char newline[] = "\r\n";
const char language_spacer_side[18] = {32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, '\0'};
const char language_spacer_top[] = "\r\n\r\n\r\n\r\n";
const char language_box_top[46] = {201,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,187,'\r', '\n', '\0'};
const char language_box_title[] = "      MOSS Kernel - Choose Language      ";
const char language_box_middle[46] = {204,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,185,'\r', '\n', '\0'}; 
const char box_side[2] = {186, '\0'};
const char language_box_bottom[46] = {200,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,188,'\r', '\n', '\0'};
const char language_option1[] = "  1. English                             ";
const char language_option2[] = "  2. suomi                               ";
char* arg_token;
int process_count = 0;
int current_process;


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

typedef struct process {
        uint32_t pid;
        struct process* parent;
        char cmd[11];
} process;


