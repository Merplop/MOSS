typedef struct task {
    long state;
    long counter;
    long priority;
    long signal;
    int exit_code;
    unsigned long end_code, end_data, brk, start_stack;
    long pid, parent, pgrp, session;
    unsigned short uid, euid, suid;
    unsigned short gid, egid, sgid;
    long alarm;
    long utime, stime, cutime, cstime, start_time;
    unsigned short used_math;
    int tty;
    unsigned short umask;
} task;