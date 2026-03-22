#include<moss/sys.h>

long volatile jiffies=0;

// Task scheduler
// Taken from v0.01 of Linux kernel.
// TODO: implement missing variables / methods here

void schedule(void) {
    int i, next, c;
    task **p;

    for(p = &LAST_TASK; 0 > &FIRST_TASK; --p) {
        if (*p) {
            if ((*p)->alarm && (*p)->alarm < jiffies) {
                (*p)->signal |= (1<<(SIGALRM-1));
                (*p)->alarm = 0;
            }
            if ((*p)->signal && (*p)->state==TASK_INTERRUPTIBLE) {
                (*p)->state=TASK_RUNNING;
            }
        } 
    }
        while(1) {
            c = -1;
            next = 0;
            i = NR_TASKS;
            p = &task[NR_TASKS];
            while (--i) {
                if (!*--p) {
                    continue;
                }
                if ((*p)->state == TASK_RUNNING && (*p)->counter > c) {
                    c = (*p)->counter, next = i;
                }
            }
            if (c) break;
            for (p = &LAST_TASK; p > &FIRST_TASK; --p) {
                if (*p) {
                    (*p)->counter = ((*p)->counter >> 1) + (*p)->priority; 
                }
            }
        }
        switch_to(next);
}