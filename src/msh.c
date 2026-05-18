/*
 * msh — MOSS Shell
 *
 * A minimal POSIX-ish shell for the MOSS operating system.
 * Supports:
 *   - Command execution via fork/execvp/waitpid
 *   - Pipes: cmd1 | cmd2 | cmd3
 *   - Redirections: >, <, >>, 2>
 *   - Variable expansion: $VAR, $?, $$
 *   - Builtins: cd, exit, export, echo, pwd, set, unset, type
 *   - Quoting: single quotes, double quotes, backslash escape
 *   - Comments: # to end of line
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#define MAX_LINE    1024
#define MAX_ARGS    128
#define MAX_PIPES   16

/* Last command exit status */
static int last_status = 0;

/* ------------------------------------------------------------------ */
/*  Token / word splitting with quoting                                */
/* ------------------------------------------------------------------ */

/*
 * Parse the next token from *src, advancing *src past it.
 * Handles single quotes, double quotes, backslash, and $VAR expansion.
 * Returns pointer to static buffer, or NULL at end of line.
 */
static char tokbuf[MAX_LINE];

static char *expand_var(const char *name) {
    if (strcmp(name, "?") == 0) {
        static char nbuf[16];
        snprintf(nbuf, sizeof(nbuf), "%d", last_status);
        return nbuf;
    }
    if (strcmp(name, "$") == 0) {
        static char pbuf[16];
        snprintf(pbuf, sizeof(pbuf), "%d", (int)getpid());
        return pbuf;
    }
    char *val = getenv(name);
    return val ? val : "";
}

static const char *next_token(const char **src) {
    const char *s = *src;
    int ti = 0;

    /* Skip whitespace */
    while (*s == ' ' || *s == '\t')
        s++;

    if (*s == '\0' || *s == '#' || *s == '\n')
        return NULL;

    /* Check for special single-character tokens */
    if (*s == '|' || *s == '<') {
        tokbuf[ti++] = *s++;
        tokbuf[ti] = '\0';
        *src = s;
        return tokbuf;
    }
    if (*s == '>') {
        tokbuf[ti++] = *s++;
        if (*s == '>') tokbuf[ti++] = *s++;  /* >> */
        tokbuf[ti] = '\0';
        *src = s;
        return tokbuf;
    }
    /* 2> redirect */
    if (*s == '2' && *(s + 1) == '>') {
        tokbuf[ti++] = *s++;
        tokbuf[ti++] = *s++;
        tokbuf[ti] = '\0';
        *src = s;
        return tokbuf;
    }

    /* Regular word */
    while (*s && *s != ' ' && *s != '\t' && *s != '\n' &&
           *s != '|' && *s != '<' && *s != '>' && *s != '#') {

        if (*s == '\\' && *(s + 1)) {
            /* Backslash escape */
            s++;
            if (ti < MAX_LINE - 1) tokbuf[ti++] = *s;
            s++;
        } else if (*s == '\'') {
            /* Single quote: literal until closing ' */
            s++;
            while (*s && *s != '\'') {
                if (ti < MAX_LINE - 1) tokbuf[ti++] = *s;
                s++;
            }
            if (*s == '\'') s++;
        } else if (*s == '"') {
            /* Double quote: allows $VAR and backslash */
            s++;
            while (*s && *s != '"') {
                if (*s == '\\' && *(s + 1)) {
                    s++;
                    if (ti < MAX_LINE - 1) tokbuf[ti++] = *s;
                    s++;
                } else if (*s == '$') {
                    s++;
                    char vname[128];
                    int vi = 0;
                    if (*s == '?' || *s == '$') {
                        vname[vi++] = *s++;
                    } else if (*s == '{') {
                        s++;
                        while (*s && *s != '}' && vi < 126)
                            vname[vi++] = *s++;
                        if (*s == '}') s++;
                    } else {
                        while ((*s >= 'a' && *s <= 'z') ||
                               (*s >= 'A' && *s <= 'Z') ||
                               (*s >= '0' && *s <= '9') || *s == '_') {
                            if (vi < 126) vname[vi++] = *s;
                            s++;
                        }
                    }
                    vname[vi] = '\0';
                    char *val = expand_var(vname);
                    while (*val && ti < MAX_LINE - 1)
                        tokbuf[ti++] = *val++;
                } else {
                    if (ti < MAX_LINE - 1) tokbuf[ti++] = *s;
                    s++;
                }
            }
            if (*s == '"') s++;
        } else if (*s == '$') {
            /* Unquoted variable expansion */
            s++;
            char vname[128];
            int vi = 0;
            if (*s == '?' || *s == '$') {
                vname[vi++] = *s++;
            } else if (*s == '{') {
                s++;
                while (*s && *s != '}' && vi < 126)
                    vname[vi++] = *s++;
                if (*s == '}') s++;
            } else {
                while ((*s >= 'a' && *s <= 'z') ||
                       (*s >= 'A' && *s <= 'Z') ||
                       (*s >= '0' && *s <= '9') || *s == '_') {
                    if (vi < 126) vname[vi++] = *s;
                    s++;
                }
            }
            vname[vi] = '\0';
            char *val = expand_var(vname);
            while (*val && ti < MAX_LINE - 1)
                tokbuf[ti++] = *val++;
        } else {
            if (ti < MAX_LINE - 1) tokbuf[ti++] = *s;
            s++;
        }
    }

    tokbuf[ti] = '\0';
    *src = s;
    return tokbuf;
}

/* ------------------------------------------------------------------ */
/*  Command structure                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char *argv[MAX_ARGS];
    int   argc;
    char *infile;       /* < redirect */
    char *outfile;      /* > or >> redirect */
    int   append;       /* 1 if >> */
    char *errfile;      /* 2> redirect */
} command_t;

/* ------------------------------------------------------------------ */
/*  Builtins                                                           */
/* ------------------------------------------------------------------ */

static int builtin_cd(command_t *cmd) {
    const char *dir = cmd->argc > 1 ? cmd->argv[1] : getenv("HOME");
    if (!dir) dir = "/";
    if (chdir(dir) < 0) {
        fprintf(stderr, "msh: cd: %s: No such file or directory\n", dir);
        return 1;
    }
    return 0;
}

static int builtin_exit(command_t *cmd) {
    int code = cmd->argc > 1 ? atoi(cmd->argv[1]) : last_status;
    exit(code);
    return 0;  /* unreachable */
}

static int builtin_export(command_t *cmd) {
    for (int i = 1; i < cmd->argc; i++) {
        char *eq = strchr(cmd->argv[i], '=');
        if (eq) {
            *eq = '\0';
            setenv(cmd->argv[i], eq + 1, 1);
            *eq = '=';
        }
    }
    return 0;
}

static int builtin_unset(command_t *cmd) {
    for (int i = 1; i < cmd->argc; i++)
        unsetenv(cmd->argv[i]);
    return 0;
}

static int builtin_pwd(command_t *cmd) {
    (void)cmd;
    char buf[256];
    if (getcwd(buf, sizeof(buf)))
        printf("%s\n", buf);
    return 0;
}

static int builtin_echo(command_t *cmd) {
    for (int i = 1; i < cmd->argc; i++) {
        if (i > 1) putchar(' ');
        fputs(cmd->argv[i], stdout);
    }
    putchar('\n');
    return 0;
}

static int builtin_type(command_t *cmd) {
    if (cmd->argc < 2) return 1;
    const char *name = cmd->argv[1];
    /* Check builtins */
    static const char *builtins[] = {
        "cd", "exit", "export", "unset", "pwd", "echo", "type", NULL
    };
    for (int i = 0; builtins[i]; i++) {
        if (strcmp(name, builtins[i]) == 0) {
            printf("%s is a shell builtin\n", name);
            return 0;
        }
    }
    /* Search PATH */
    char *path = getenv("PATH");
    if (path) {
        char pathbuf[256];
        char trial[512];
        strncpy(pathbuf, path, sizeof(pathbuf) - 1);
        pathbuf[sizeof(pathbuf) - 1] = '\0';
        char *dir = pathbuf;
        while (dir) {
            char *colon = strchr(dir, ':');
            if (colon) *colon = '\0';
            snprintf(trial, sizeof(trial), "%s/%s", dir, name);
            if (access(trial, F_OK) == 0) {
                printf("%s is %s\n", name, trial);
                return 0;
            }
            dir = colon ? colon + 1 : NULL;
        }
    }
    printf("msh: type: %s: not found\n", name);
    return 1;
}

typedef int (*builtin_fn_t)(command_t *);

static struct {
    const char *name;
    builtin_fn_t fn;
} builtin_table[] = {
    { "cd",     builtin_cd },
    { "exit",   builtin_exit },
    { "export", builtin_export },
    { "unset",  builtin_unset },
    { "pwd",    builtin_pwd },
    { "echo",   builtin_echo },
    { "type",   builtin_type },
    { NULL,     NULL }
};

static builtin_fn_t find_builtin(const char *name) {
    for (int i = 0; builtin_table[i].name; i++)
        if (strcmp(name, builtin_table[i].name) == 0)
            return builtin_table[i].fn;
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Parse a pipeline from the input line                               */
/* ------------------------------------------------------------------ */

static int parse_pipeline(const char *line, command_t *cmds, int *ncmds) {
    *ncmds = 0;
    int ci = 0;

    memset(&cmds[ci], 0, sizeof(command_t));

    while (1) {
        const char *tok = next_token(&line);
        if (!tok) break;

        if (strcmp(tok, "|") == 0) {
            /* End current command, start new one */
            cmds[ci].argv[cmds[ci].argc] = NULL;
            ci++;
            if (ci >= MAX_PIPES) {
                fprintf(stderr, "msh: too many pipes\n");
                return -1;
            }
            memset(&cmds[ci], 0, sizeof(command_t));
        } else if (strcmp(tok, "<") == 0) {
            const char *file = next_token(&line);
            if (!file) { fprintf(stderr, "msh: expected filename after <\n"); return -1; }
            cmds[ci].infile = strdup(file);
        } else if (strcmp(tok, ">") == 0) {
            const char *file = next_token(&line);
            if (!file) { fprintf(stderr, "msh: expected filename after >\n"); return -1; }
            cmds[ci].outfile = strdup(file);
            cmds[ci].append = 0;
        } else if (strcmp(tok, ">>") == 0) {
            const char *file = next_token(&line);
            if (!file) { fprintf(stderr, "msh: expected filename after >>\n"); return -1; }
            cmds[ci].outfile = strdup(file);
            cmds[ci].append = 1;
        } else if (strcmp(tok, "2>") == 0) {
            const char *file = next_token(&line);
            if (!file) { fprintf(stderr, "msh: expected filename after 2>\n"); return -1; }
            cmds[ci].errfile = strdup(file);
        } else {
            /* Regular argument */
            if (cmds[ci].argc < MAX_ARGS - 1)
                cmds[ci].argv[cmds[ci].argc++] = strdup(tok);
        }
    }

    cmds[ci].argv[cmds[ci].argc] = NULL;
    *ncmds = ci + 1;

    /* Check for empty commands */
    if (cmds[0].argc == 0) {
        *ncmds = 0;
        return 0;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Execute a pipeline                                                 */
/* ------------------------------------------------------------------ */

static void exec_pipeline(command_t *cmds, int ncmds) {
    if (ncmds == 0) return;

    /* Single command — check for builtin */
    if (ncmds == 1 && !cmds[0].infile && !cmds[0].outfile) {
        builtin_fn_t bi = find_builtin(cmds[0].argv[0]);
        if (bi) {
            last_status = bi(&cmds[0]);
            return;
        }
    }

    int prev_pipe_read = -1;  /* read end of previous pipe */

    for (int i = 0; i < ncmds; i++) {
        int pipefd[2] = { -1, -1 };

        /* Create pipe to next command (if not last) */
        if (i < ncmds - 1) {
            if (pipe(pipefd) < 0) {
                fprintf(stderr, "msh: pipe failed\n");
                last_status = 1;
                return;
            }
        }

        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "msh: fork failed\n");
            last_status = 1;
            return;
        }

        if (pid == 0) {
            /* Child process */

            /* Set up stdin from previous pipe */
            if (prev_pipe_read >= 0) {
                dup2(prev_pipe_read, STDIN_FILENO);
                close(prev_pipe_read);
            }

            /* Set up stdout to next pipe */
            if (pipefd[1] >= 0) {
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }
            if (pipefd[0] >= 0) close(pipefd[0]);

            /* Input redirect */
            if (cmds[i].infile) {
                int fd = open(cmds[i].infile, O_RDONLY);
                if (fd < 0) {
                    fprintf(stderr, "msh: %s: No such file\n", cmds[i].infile);
                    _exit(1);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            /* Output redirect */
            if (cmds[i].outfile) {
                int flags = O_WRONLY | O_CREAT;
                if (cmds[i].append)
                    flags |= O_APPEND;
                else
                    flags |= O_TRUNC;
                int fd = open(cmds[i].outfile, flags);
                if (fd < 0) {
                    fprintf(stderr, "msh: %s: Cannot open for writing\n", cmds[i].outfile);
                    _exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            /* Stderr redirect */
            if (cmds[i].errfile) {
                int fd = open(cmds[i].errfile, O_WRONLY | O_CREAT | O_TRUNC);
                if (fd < 0) _exit(1);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }

            /* Check for builtin in pipeline context */
            builtin_fn_t bi = find_builtin(cmds[i].argv[0]);
            if (bi) {
                _exit(bi(&cmds[i]));
            }

            /* Execute external command */
            execvp(cmds[i].argv[0], cmds[i].argv);
            fprintf(stderr, "msh: %s: command not found\n", cmds[i].argv[0]);
            _exit(127);
        }

        /* Parent: close pipe write end and previous read end */
        if (prev_pipe_read >= 0) close(prev_pipe_read);
        if (pipefd[1] >= 0) close(pipefd[1]);
        prev_pipe_read = pipefd[0];
    }

    /* Wait for all children */
    int status;
    for (int i = 0; i < ncmds; i++) {
        waitpid(-1, &status, 0);
    }
    /* Use last child's status */
    last_status = status;
}

/* ------------------------------------------------------------------ */
/*  Free parsed commands                                               */
/* ------------------------------------------------------------------ */

static void free_commands(command_t *cmds, int ncmds) {
    for (int i = 0; i < ncmds; i++) {
        for (int j = 0; j < cmds[i].argc; j++)
            free(cmds[i].argv[j]);
        free(cmds[i].infile);
        free(cmds[i].outfile);
        free(cmds[i].errfile);
    }
}

/* ------------------------------------------------------------------ */
/*  Main loop                                                          */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    char line[MAX_LINE];
    command_t cmds[MAX_PIPES];
    int ncmds;
    int interactive = isatty(STDIN_FILENO);

    /* Ignore SIGINT in the shell itself (children will get it) */
    signal(SIGINT, SIG_IGN);

    while (1) {
        if (interactive) {
            char cwd[128];
            if (getcwd(cwd, sizeof(cwd)))
                fprintf(stderr, "%s$ ", cwd);
            else
                fprintf(stderr, "$ ");
        }

        /* Read a line */
        int len = 0;
        int n = read(STDIN_FILENO, line, MAX_LINE - 1);
        if (n <= 0) break;  /* EOF */
        len = n;
        line[len] = '\0';

        /* Strip trailing newline */
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';

        /* Skip empty lines */
        if (len == 0) continue;

        /* Parse and execute */
        if (parse_pipeline(line, cmds, &ncmds) == 0) {
            exec_pipeline(cmds, ncmds);
            free_commands(cmds, ncmds);
        }
    }

    return last_status;
}
