#define _POSIX_C_SOURCE 200809L
#include "courier.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

void *courier_thread_func(void *arg)
{
    CourierContext *ctx = (CourierContext *)arg;
    if (!ctx)
        return NULL;

    while (1)
    {
        Order order;

        /* Lock the queue */
        pthread_mutex_lock(ctx->queue_mutex);

        /* Wait while queue is empty and not shutting down */
        while (pq_is_empty(ctx->queue) && !atomic_load(ctx->shutdown_flag))
        {
            /* Log WAITING */
            pthread_mutex_lock(ctx->log_mutex);
            printf("[COURIER-%u] WAITING\n", ctx->courier_id);
            pthread_mutex_unlock(ctx->log_mutex);

            /* Wait for signal or new order */
            pthread_cond_wait(ctx->queue_cond, ctx->queue_mutex);
        }

        /* Check if shutting down or queue empty */
        if (pq_is_empty(ctx->queue) || atomic_load(ctx->shutdown_flag))
        {
            pthread_mutex_unlock(ctx->queue_mutex);
            break;
        }

        /* Dequeue the next order */
        if (!pq_dequeue(ctx->queue, &order))
        {
            pthread_mutex_unlock(ctx->queue_mutex);
            break;
        }

        atomic_fetch_add(ctx->active_deliveries, 1);
        pthread_mutex_unlock(ctx->queue_mutex);

        /* Log DELIVERY_START */
        pthread_mutex_lock(ctx->log_mutex);
        printf("[COURIER-%u] DELIVERY_START id=%u recipient=%s priority=%s\n",
               ctx->courier_id, order.id, order.name, priority_to_string(order.priority));
        pthread_mutex_unlock(ctx->log_mutex);

        /* Simulate delivery: 1 unit = 100 ms */
        uint64_t duration_ms = (uint64_t)order.duration * 100;
        struct timespec ts = {
            .tv_sec = (time_t)(duration_ms / 1000),
            .tv_nsec = (long)((duration_ms % 1000) * 1000000)};
        nanosleep(&ts, NULL);

        /* Log DELIVERY_COMPLETE */
        pthread_mutex_lock(ctx->log_mutex);
        printf("[COURIER-%u] DELIVERY_COMPLETE id=%u recipient=%s duration=%lu ms\n",
               ctx->courier_id, order.id, order.name, (unsigned long)duration_ms);
        pthread_mutex_unlock(ctx->log_mutex);

        /* Update global stats */
        stats_increment_completed(ctx->stats);
        stats_add_delivery_time(ctx->stats, duration_ms);
        stats_record_courier_completion(ctx->stats, ctx->courier_id, duration_ms);

        atomic_fetch_sub(ctx->active_deliveries, 1);
        pthread_mutex_lock(ctx->queue_mutex);
        pthread_cond_broadcast(ctx->done_cond);
        pthread_mutex_unlock(ctx->queue_mutex);
    }

    /* Log SHIFT_OVER */
    pthread_mutex_lock(ctx->log_mutex);
    printf("[COURIER-%u] SHIFT_OVER\n", ctx->courier_id);
    pthread_mutex_unlock(ctx->log_mutex);

    free(ctx);
    return NULL;
}
