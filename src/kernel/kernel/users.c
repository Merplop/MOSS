// MOSS KERNEL - User Management
// (C) Miro Haapalainen, 2024

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <kernel/users.h>
#include <kernel/sched.h>
#include <kernel/keyboard.h>
#include <kernel/tty.h>
#include <kernel/kernel.h>

/* ------------------------------------------------------------------ */
/*  String helpers (kernel libc doesn't provide strcmp/strncpy)         */
/* ------------------------------------------------------------------ */

static int str_cmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void str_ncpy(char *dst, const char *src, int n) {
    int i;
    for (i = 0; i < n - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ------------------------------------------------------------------ */
/*  In-kernel user database                                            */
/* ------------------------------------------------------------------ */

static user_entry_t user_table[MAX_USERS];
static int num_users = 0;

void users_init(void) {
    memset(user_table, 0, sizeof(user_table));
    num_users = 0;

    /* Create default root user */
    user_add("root", "root", UID_ROOT, 0, "/");
}

user_entry_t *user_lookup(const char *username) {
    if (!username)
        return NULL;
    for (int i = 0; i < num_users; i++) {
        if (str_cmp(user_table[i].username, username) == 0)
            return &user_table[i];
    }
    return NULL;
}

user_entry_t *user_lookup_uid(uint16_t uid) {
    for (int i = 0; i < num_users; i++) {
        if (user_table[i].uid == uid)
            return &user_table[i];
    }
    return NULL;
}

int user_authenticate(const char *username, const char *password) {
    user_entry_t *u = user_lookup(username);
    if (!u)
        return 0;
    return (str_cmp(u->password, password) == 0) ? 1 : 0;
}

int user_add(const char *username, const char *password, uint16_t uid, uint16_t gid, const char *home) {
    if (num_users >= MAX_USERS)
        return -1;
    if (!username || !password)
        return -1;
    if (user_lookup(username) != NULL)
        return -1;  /* user already exists */
    if (user_lookup_uid(uid) != NULL)
        return -1;  /* uid already taken */

    user_entry_t *u = &user_table[num_users];
    memset(u, 0, sizeof(*u));
    u->uid = uid;
    u->gid = gid;
    str_ncpy(u->username, username, MAX_USERNAME - 1);
    str_ncpy(u->password, password, MAX_PASSWORD - 1);
    if (home)
        str_ncpy(u->home, home, sizeof(u->home) - 1);
    else
        str_ncpy(u->home, "/", sizeof(u->home) - 1);

    num_users++;
    return 0;
}

int user_remove(const char *username) {
    if (!username)
        return -1;
    /* Don't allow removing root */
    if (str_cmp(username, "root") == 0)
        return -1;

    for (int i = 0; i < num_users; i++) {
        if (str_cmp(user_table[i].username, username) == 0) {
            /* Shift remaining entries down */
            for (int j = i; j < num_users - 1; j++)
                user_table[j] = user_table[j + 1];
            memset(&user_table[num_users - 1], 0, sizeof(user_entry_t));
            num_users--;
            return 0;
        }
    }
    return -1;
}

int user_change_password(const char *username, const char *old_pass, const char *new_pass) {
    user_entry_t *u = user_lookup(username);
    if (!u)
        return -1;
    /* Verify old password (unless caller is root) */
    task_t *t = get_current_task();
    if (t && t->uid != UID_ROOT) {
        if (str_cmp(u->password, old_pass) != 0)
            return -1;
    }
    memset(u->password, 0, MAX_PASSWORD);
    str_ncpy(u->password, new_pass, MAX_PASSWORD - 1);
    return 0;
}

int user_count(void) {
    return num_users;
}

user_entry_t *user_get_by_index(int index) {
    if (index < 0 || index >= num_users)
        return NULL;
    return &user_table[index];
}

const char *user_current_name(void) {
    task_t *t = get_current_task();
    if (!t)
        return "unknown";
    user_entry_t *u = user_lookup_uid(t->uid);
    if (!u)
        return "unknown";
    return u->username;
}

/* ------------------------------------------------------------------ */
/*  Permission checking                                                */
/* ------------------------------------------------------------------ */

int check_permission(uint16_t uid, uint16_t gid,
                     uint16_t file_uid, uint16_t file_gid,
                     uint16_t file_mode, int access) {
    /* Root can do anything */
    if (uid == UID_ROOT)
        return 1;

    uint16_t perm;

    if (uid == file_uid) {
        /* Owner permissions (bits 8-6) */
        perm = (file_mode >> 6) & 7;
    } else if (gid == file_gid) {
        /* Group permissions (bits 5-3) */
        perm = (file_mode >> 3) & 7;
    } else {
        /* Other permissions (bits 2-0) */
        perm = file_mode & 7;
    }

    /* Check requested access against available permissions */
    if ((access & R_OK) && !(perm & 4))
        return 0;
    if ((access & W_OK) && !(perm & 2))
        return 0;
    if ((access & X_OK) && !(perm & 1))
        return 0;

    return 1;
}

/* ------------------------------------------------------------------ */
/*  Login prompt                                                       */
/* ------------------------------------------------------------------ */

void login_prompt(void) {
    char username_buf[MAX_USERNAME];
    char password_buf[MAX_PASSWORD];
    int pos;
    uint8_t ch;

    while (1) {
        printf("\r\nMOSS login: ");
        memset(username_buf, 0, sizeof(username_buf));
        pos = 0;

        /* Read username */
        while (1) {
            ch = get_key();
            if (ch == 0x0D) {  /* Enter */
                printf("\r\n");
                break;
            }
            if (ch == 0x08) {  /* Backspace */
                if (pos > 0) {
                    username_buf[--pos] = '\0';
                    putchar(ch);
                }
                continue;
            }
            if (pos < MAX_USERNAME - 1 && ch >= 0x20 && ch <= 0x7E) {
                username_buf[pos++] = ch;
                putchar(ch);
            }
        }

        if (pos == 0)
            continue;

        printf("Password: ");
        memset(password_buf, 0, sizeof(password_buf));
        pos = 0;

        /* Read password (no echo) */
        while (1) {
            ch = get_key();
            if (ch == 0x0D) {  /* Enter */
                printf("\r\n");
                break;
            }
            if (ch == 0x08) {  /* Backspace */
                if (pos > 0) {
                    password_buf[--pos] = '\0';
                }
                continue;
            }
            if (pos < MAX_PASSWORD - 1 && ch >= 0x20 && ch <= 0x7E) {
                password_buf[pos++] = ch;
            }
        }

        if (user_authenticate(username_buf, password_buf)) {
            /* Set current task credentials */
            user_entry_t *u = user_lookup(username_buf);
            task_t *t = get_current_task();
            if (t && u) {
                t->uid  = u->uid;
                t->gid  = u->gid;
                t->euid = u->uid;
                t->egid = u->gid;
            }
            printf("Welcome, %s!\r\n", username_buf);
            return;
        } else {
            printf("Login incorrect\r\n");
        }
    }
}
