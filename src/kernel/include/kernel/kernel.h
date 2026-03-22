#ifndef _KERNEL_KERNEL_H
#define _KERNEL_KERNEL_H

#include<stdint.h>
#include<stddef.h>

#define MAX_FILE_SIZE 8192
#define MAX_DIR_CAPACITY 8192
#define NUM_COMMANDS 21
#define MAX_ARGS 10

extern const char KERNEL_PANIC_ERROR[];
extern const char CMD_ERROR[];
extern const char ARG_COUNT_ERROR[];
extern const char ARG_ERROR[];
extern const char INODE_FULL_ERROR[];
extern const char DIR_EXISTS_ERROR[];
extern const char FILE_EXISTS_ERROR[];
extern const char FILE_NOT_FOUND_ERROR[];
extern const char FILE_EMPTY_ERROR[];
extern const char DIR_NOT_FOUND_ERROR[];
extern const char KERNEL_ERROR_FIN[];
extern const char CMD_ERROR_FIN[];
extern const char ARG_COUNT_ERROR_FIN[];

extern const char version[];
extern const char welcome1_eng[];
extern const char welcome1_fin[];
extern const char welcome2[];
extern const char prompt[];
extern char cmd_str[256];
extern char *cmd_str_ptr;
extern uint8_t input_char;
extern uint8_t input_length;
extern int argc;
extern char *argv[MAX_ARGS];
extern int language;
extern const char newline[];
extern const char language_spacer_side[18];
extern const char language_spacer_top[];
extern const char language_box_top[46];
extern const char language_box_title[];
extern const char language_box_middle[46];
extern const char box_side[2];
extern const char language_box_bottom[46];
extern const char language_option1[];
extern const char language_option2[];
extern char* arg_token;
extern int process_count;
extern int current_process;


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
} Inode;

typedef struct process {
        uint32_t pid;
        struct process* parent;
        char cmd[11];
} process;

/* Functions provided by shell.c */
void language_prompt(void);
void start_shell(void);

/* Functions provided by kernel.c */
void panic(void);

int atoi(const char *s);

#endif