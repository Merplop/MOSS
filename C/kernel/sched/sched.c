/* 
* The MOSS Kernel's Round-Robin Scheduler
* File: kernel/sched/sched.c
* Miro Haapalainen, 2025
*/

#include <kernel/sched/sched.h>
#include <kernel/mm/kmalloc.h>

sched_list_t *sched_list_head = NULL;
static int current_task_index = 0;


typedef struct task_s {
    long state;
    long counter;
    long priority;
    long signal;
    int exit_code;

} task_t;

typedef struct sched_list_s {
    task_t t;
    struct sched_list_s *next;
    struct sched_list_s *prev;
} sched_list_t;

void admit_task(task_t *new_task) {
    sched_list_t *new_node = (sched_list_t *)kmalloc(sizeof(sched_list_t));
    new_node->t = *new_task;
    
    if (sched_list_head == NULL) {
        sched_list_head = new_node;
        new_node->next = new_node;
        new_node->prev = new_node;
    } else {
        sched_list_t *tail = sched_list_head->prev;
        tail->next = new_node;
        new_node->prev = tail;
        new_node->next = sched_list_head;
        sched_list_head->prev = new_node;
    }
    current_task_index++;
}

void remove_task(task_t *task_to_remove) {
    if (sched_list_head == NULL) return;

    sched_list_t *current = sched_list_head;
    do {
        if (&current->t == task_to_remove) {
            if (current->next == current) {
                sched_list_head = NULL;
            } else {
                current->prev->next = current->next;
                current->next->prev = current->prev;
                if (sched_list_head == current) {
                    sched_list_head = current->next;
                }
            }
            kfree(current);
            current_task_index--;
            return;
        }
        current = current->next;
    } while (current != sched_list_head);
}

