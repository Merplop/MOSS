#ifndef _KERNEL_USERS_H
#define _KERNEL_USERS_H

#include <stdint.h>

/* Maximum number of registered users */
#define MAX_USERS       16
#define MAX_USERNAME    32
#define MAX_PASSWORD    64

/* Special user IDs */
#define UID_ROOT        0

/* User entry in the kernel user database */
typedef struct {
    uint16_t uid;
    uint16_t gid;
    char     username[MAX_USERNAME];
    char     password[MAX_PASSWORD];   /* plaintext for now (simple OS) */
    char     home[64];                 /* home directory path */
} user_entry_t;

/* Initialise the user subsystem (creates root user if none exist) */
void users_init(void);

/* Look up a user by username. Returns pointer or NULL. */
user_entry_t *user_lookup(const char *username);

/* Look up a user by uid. Returns pointer or NULL. */
user_entry_t *user_lookup_uid(uint16_t uid);

/* Authenticate: returns 1 on success, 0 on failure. */
int user_authenticate(const char *username, const char *password);

/* Add a new user. Returns 0 on success, -1 on failure. */
int user_add(const char *username, const char *password, uint16_t uid, uint16_t gid, const char *home);

/* Remove a user by username. Returns 0 on success, -1 on failure. */
int user_remove(const char *username);

/* Change a user's password. Returns 0 on success, -1 on failure. */
int user_change_password(const char *username, const char *old_pass, const char *new_pass);

/* Get the number of registered users. */
int user_count(void);

/* Get user entry by index (for listing). Returns NULL if out of range. */
user_entry_t *user_get_by_index(int index);

/* Get the currently logged-in username for the running task. */
const char *user_current_name(void);

/* Present a login prompt and set credentials on the current task. */
void login_prompt(void);

/* Check file permission.
 * Returns 1 if access is allowed, 0 if denied.
 *   uid/gid: requesting process credentials
 *   file_uid/file_gid: owner of the file (from inode)
 *   file_mode: permission bits (lower 12 bits of i_mode)
 *   access: requested access (combination of R_OK, W_OK, X_OK)
 */
#define R_OK 4
#define W_OK 2
#define X_OK 1

int check_permission(uint16_t uid, uint16_t gid,
                     uint16_t file_uid, uint16_t file_gid,
                     uint16_t file_mode, int access);

#endif /* _KERNEL_USERS_H */
