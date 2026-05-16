#ifndef COURIER_H
#define COURIER_H

#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include "order.h"
#include "priority_queue.h"
#include "stats.h"

typedef struct
{
    /* Shared resources */
    PriorityQueue *queue;
    pthread_mutex_t *queue_mutex;
    pthread_cond_t *queue_cond;
    pthread_mutex_t *log_mutex;

    GlobalStats *stats;

    /* Courier ID (1-based) */
    uint32_t courier_id;

    /* Shutdown flag */
    _Atomic(int) *shutdown_flag;

    /* Active delivery counter */
    _Atomic(uint32_t) *active_deliveries;

    /* Completion condition variable */
    pthread_cond_t *done_cond;

} CourierContext;

/* Courier thread entry point */
void *courier_thread_func(void *arg);

#endif
