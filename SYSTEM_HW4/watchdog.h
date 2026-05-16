#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <signal.h>
#include <sys/types.h>

typedef struct
{
    int *pipe_read_fds; /* one per Reader */
    int num_readers;
    pid_t *all_child_pids; /* all child PIDs (readers + dispatcher + analyzers + aggregator) */
    int num_children;
    volatile sig_atomic_t *shutdown_flag; /* set to 1 by parent to stop watchdog */
} watchdog_config_t;

void *watchdog_thread_func(void *arg);

#endif /* WATCHDOG_H */
