#define _GNU_SOURCE
#include "dispatcher.h"
#include "shm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

static int is_priority(const char *source, char **filters, int num_filters)
{
    for (int i = 0; i < num_filters; i++)
    {
        if (strcmp(source, filters[i]) == 0)
            return 1;
    }
    return 0;
}

static void push_to_region_b(int level_idx, const log_entry_t *e)
{
    pthread_mutex_lock(&shm_b[level_idx]->level_mutex);
    while (cb_full(shm_b[level_idx]->head, shm_b[level_idx]->tail, shm_b[level_idx]->capacity))
        pthread_cond_wait(&shm_b[level_idx]->not_full_b, &shm_b[level_idx]->level_mutex);
    shm_b_buf[level_idx][shm_b[level_idx]->head] = *e;
    shm_b[level_idx]->head = (shm_b[level_idx]->head + 1) % shm_b[level_idx]->capacity;
    pthread_cond_signal(&shm_b[level_idx]->not_empty_b);
    pthread_mutex_unlock(&shm_b[level_idx]->level_mutex);
}

static void push_to_region_d(const log_entry_t *e)
{
    pthread_mutex_lock(&shm_d->priority_mutex);
    while (cb_full(shm_d->head, shm_d->tail, shm_d->capacity))
        pthread_cond_wait(&shm_d->not_full_d, &shm_d->priority_mutex);
    shm_d_buf[shm_d->head] = *e;
    shm_d->head = (shm_d->head + 1) % shm_d->capacity;
    pthread_cond_signal(&shm_d->not_empty_d);
    pthread_mutex_unlock(&shm_d->priority_mutex);
}

void run_dispatcher(const dispatcher_config_t *cfg)
{
    printf("[PID:%d] Dispatcher started.\n", (int)getpid());
    fflush(stdout);

    int eof_forwarded[4] = {0, 0, 0, 0};
    int eof_seen[4] = {0, 0, 0, 0};
    int all_done = 0;

    while (!all_done)
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += cfg->timeout_sec;

        pthread_mutex_lock(&shm_a->input_mutex);
        int rc = 0;
        while (cb_empty(shm_a->head, shm_a->tail) && rc == 0)
        {
            rc = pthread_cond_timedwait(&shm_a->not_empty_a, &shm_a->input_mutex, &ts);
        }

        if (cb_empty(shm_a->head, shm_a->tail))
        {
            /* Timeout: check if all EOFs seen */
            int all_seen = 1;
            for (int l = 0; l < 4; l++)
            {
                if (eof_seen[l] < shm_a->total_readers)
                {
                    all_seen = 0;
                    break;
                }
            }
            pthread_mutex_unlock(&shm_a->input_mutex);
            if (all_seen)
                break;
            continue;
        }

        log_entry_t e = shm_a_buf[shm_a->tail];
        shm_a->tail = (shm_a->tail + 1) % shm_a->capacity;
        pthread_cond_signal(&shm_a->not_full_a);
        pthread_mutex_unlock(&shm_a->input_mutex);

        /* EOF marker: level_index carries the completed severity. */
        if (e.is_eof)
        {
            int l = e.level_index; /* 0..3 */
            if (l >= 0 && l < 4)
            {
                eof_seen[l]++;

                if (eof_seen[l] >= cfg->total_readers && !eof_forwarded[l])
                {
                    /* Forward EOF to Region B[l] */
                    log_entry_t eof_marker;
                    memset(&eof_marker, 0, sizeof(eof_marker));
                    eof_marker.is_eof = 1;
                    eof_marker.level_index = l;

                    push_to_region_b(l, &eof_marker);

                    pthread_mutex_lock(&shm_b[l]->level_mutex);
                    shm_b[l]->eof_posted = 1;
                    pthread_cond_broadcast(&shm_b[l]->not_empty_b);
                    pthread_mutex_unlock(&shm_b[l]->level_mutex);

                    eof_forwarded[l] = 1;
                }
            }
            /* Check if all 4 levels forwarded */
            all_done = (eof_forwarded[0] && eof_forwarded[1] &&
                        eof_forwarded[2] && eof_forwarded[3]);
            continue;
        }

        int hi = is_priority(e.source, cfg->filter_sources, cfg->num_filters);

        const char *level_names[] = {"ERROR", "WARN", "INFO", "DEBUG"};
        printf("[PID:%d] Routed entry to %s buffer. High-priority: %s (source: %s)\n",
               (int)getpid(),
               (e.level_index >= 0 && e.level_index < 4) ? level_names[e.level_index] : "UNKNOWN",
               hi ? "YES" : "NO",
               e.source);
        fflush(stdout);

        /* Route to Region B */
        if (e.level_index >= 0 && e.level_index < 4)
        {
            push_to_region_b(e.level_index, &e);
        }

        /* If high-priority, also copy to Region D */
        if (hi)
        {
            push_to_region_d(&e);
        }

        all_done = (eof_forwarded[0] && eof_forwarded[1] &&
                    eof_forwarded[2] && eof_forwarded[3]);
    }

    /* Ensure all level EOFs forwarded after all EOF markers were consumed. */
    for (int l = 0; l < 4; l++)
    {
        if (!eof_forwarded[l] && eof_seen[l] >= cfg->total_readers)
        {
            log_entry_t eof_marker;
            memset(&eof_marker, 0, sizeof(eof_marker));
            eof_marker.is_eof = 1;
            eof_marker.level_index = l;
            push_to_region_b(l, &eof_marker);
            pthread_mutex_lock(&shm_b[l]->level_mutex);
            shm_b[l]->eof_posted = 1;
            pthread_cond_broadcast(&shm_b[l]->not_empty_b);
            pthread_mutex_unlock(&shm_b[l]->level_mutex);
        }
    }

    /* Signal Region D done */
    pthread_mutex_lock(&shm_d->priority_mutex);
    shm_d->dispatcher_done = 1;
    pthread_cond_broadcast(&shm_d->not_empty_d);
    pthread_mutex_unlock(&shm_d->priority_mutex);

    printf("[PID:%d] All EOF markers forwarded to Region B. Exiting.\n", (int)getpid());
    fflush(stdout);
    _exit(0);
}
