#define _GNU_SOURCE
#include "aggregator.h"
#include "shm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

#define MAGIC 0xC5E3440B
#define VERSION 1

/* Drain currently available high-priority entries from Region D. */
static void drain_hp_available(const aggregator_config_t *cfg, double *total)
{
    const int weights[] = {4, 3, 2, 1};

    pthread_mutex_lock(&shm_d->priority_mutex);
    while (!cb_empty(shm_d->head, shm_d->tail))
    {
        log_entry_t e = shm_d_buf[shm_d->tail];
        shm_d->tail = (shm_d->tail + 1) % shm_d->capacity;
        pthread_cond_signal(&shm_d->not_full_d);

        if (e.level_index >= 0 && e.level_index < 4)
        {
            int w = weights[e.level_index];
            for (int k = 0; k < cfg->num_keywords; k++)
            {
                int cnt = 0;
                int klen = (int)strlen(cfg->keywords[k]);
                if (klen > 0)
                {
                    for (int i = 0; e.message[i]; i++)
                    {
                        if (strncmp(e.message + i, cfg->keywords[k], (size_t)klen) == 0)
                            cnt++;
                    }
                }
                *total += (double)cnt * (double)w;
            }
        }
    }
    pthread_mutex_unlock(&shm_d->priority_mutex);
}

static int hp_done_and_empty(void)
{
    int done;

    pthread_mutex_lock(&shm_d->priority_mutex);
    done = shm_d->dispatcher_done && cb_empty(shm_d->head, shm_d->tail);
    pthread_mutex_unlock(&shm_d->priority_mutex);
    return done;
}

static int cmp_results_desc(const void *a, const void *b)
{
    const level_result_t *ra = (const level_result_t *)a;
    const level_result_t *rb = (const level_result_t *)b;
    if (rb->total_weighted_score > ra->total_weighted_score)
        return 1;
    if (rb->total_weighted_score < ra->total_weighted_score)
        return -1;
    return 0;
}

void run_aggregator(const aggregator_config_t *cfg)
{
    printf("[PID:%d] Aggregator started. Waiting for 4 levels...\n", (int)getpid());
    fflush(stdout);

    const char *lnames[] = {"ERROR", "WARN", "INFO", "DEBUG"};

    int received[4] = {0, 0, 0, 0};
    int received_count = 0;
    double hp_score = 0.0;

    while (received_count < 4)
    {
        int newly_received[4] = {0, 0, 0, 0};

        drain_hp_available(cfg, &hp_score);

        pthread_mutex_lock(&shm_c->result_mutex);
        for (int i = 0; i < 4; i++)
        {
            if (!received[i] && shm_c->results[i].ready)
            {
                received[i] = 1;
                received_count++;
                while (sem_trywait(&shm_c->result_sem[i]) == 0)
                {
                }
                newly_received[i] = 1;
            }
        }

        if (received_count < 4)
        {
            int wait_sec = cfg->timeout_sec;
            if (wait_sec > 1)
                wait_sec = 1;
            if (wait_sec < 1)
                wait_sec = 1;

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += wait_sec;
            int rc = pthread_cond_timedwait(&shm_c->result_cond,
                                            &shm_c->result_mutex, &ts);
            if (rc != 0 && rc != ETIMEDOUT)
            {
                fprintf(stderr, "[PID:%d] Aggregator: result wait error: %s\n",
                        (int)getpid(), strerror(rc));
            }
        }
        pthread_mutex_unlock(&shm_c->result_mutex);

        for (int i = 0; i < 4; i++)
        {
            if (newly_received[i])
            {
                printf("[PID:%d] %s result received.\n", (int)getpid(), lnames[i]);
                fflush(stdout);
            }
        }
    }

    while (!hp_done_and_empty())
    {
        drain_hp_available(cfg, &hp_score);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_mutex_lock(&shm_d->priority_mutex);
        if (!shm_d->dispatcher_done && cb_empty(shm_d->head, shm_d->tail))
            (void)pthread_cond_timedwait(&shm_d->not_empty_d, &shm_d->priority_mutex, &ts);
        pthread_mutex_unlock(&shm_d->priority_mutex);
    }
    drain_hp_available(cfg, &hp_score);

    printf("[PID:%d] All results received. Writing output files...\n", (int)getpid());
    fflush(stdout);

    pthread_mutex_lock(&shm_c->result_mutex);
    shm_c->high_priority_score = hp_score;
    pthread_mutex_unlock(&shm_c->result_mutex);

    /* Copy results and sort by weighted score DESC */
    level_result_t sorted[4];
    for (int i = 0; i < 4; i++)
        sorted[i] = shm_c->results[i];
    qsort(sorted, 4, sizeof(level_result_t), cmp_results_desc);

    double total_weighted = 0.0;
    long total_entries = 0;
    for (int i = 0; i < 4; i++)
    {
        total_weighted += sorted[i].total_weighted_score;
        total_entries += sorted[i].total_entries;
    }

    /* ---- Write human-readable output ---- */
    FILE *out = fopen(cfg->output_path, "w");
    if (!out)
    {
        fprintf(stderr, "Cannot open output file %s: %s\n", cfg->output_path, strerror(errno));
        _exit(1);
    }

    /* Header */
    fprintf(out, "KEYWORD_LIST: ");
    for (int k = 0; k < cfg->num_keywords; k++)
    {
        if (k > 0)
            fprintf(out, ",");
        fprintf(out, "%s", cfg->keywords[k]);
    }
    fprintf(out, "\n");
    fprintf(out, "FILES: %d\n", cfg->num_files);
    fprintf(out, "TOTAL_WEIGHTED_SCORE: %.1f\n", total_weighted);
    fprintf(out, "HIGH_PRIORITY_SCORE: %.1f\n", hp_score);

    /* Level table header */
    fprintf(out, "# Levels sorted by total_weighted_score DESC\n");
    fprintf(out, "LEVEL  ENTRIES  WEIGHTED_SCORE");
    for (int k = 0; k < cfg->num_keywords; k++)
        fprintf(out, "  %s", cfg->keywords[k]);
    fprintf(out, "\n");

    for (int i = 0; i < 4; i++)
    {
        fprintf(out, "%-6s  %7ld  %13.1f",
                sorted[i].level,
                sorted[i].total_entries,
                sorted[i].total_weighted_score);
        for (int k = 0; k < cfg->num_keywords; k++)
            fprintf(out, "  %6.1f", sorted[i].per_keyword_score[k]);
        fprintf(out, "\n");
    }

    /* Top-3 sources */
    fprintf(out, "# Top-3 sources per level\n");
    for (int i = 0; i < 4; i++)
    {
        fprintf(out, "%-6s", sorted[i].level);
        for (int s = 0; s < 3; s++)
        {
            if (sorted[i].top_source[s][0] != '\0')
                fprintf(out, "  %s:%ld", sorted[i].top_source[s], sorted[i].top_source_hits[s]);
        }
        fprintf(out, "\n");
    }

    /* Per-thread contributions */
    fprintf(out, "# Per-thread contributions (weighted score)\n");
    for (int i = 0; i < 4; i++)
    {
        fprintf(out, "%-6s", sorted[i].level);
        for (int w = 0; w < cfg->num_workers; w++)
        {
            if (sorted[i].per_thread_score[w] != 0.0)
                fprintf(out, "  thread_%d:%.1f", w, sorted[i].per_thread_score[w]);
        }
        fprintf(out, "\n");
    }

    fclose(out);

    /* ---- Write binary checkpoint (atomic) ---- */
    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", cfg->binary_path);

    FILE *bin = fopen(tmp_path, "wb");
    if (!bin)
    {
        fprintf(stderr, "Cannot open binary tmp file %s: %s\n", tmp_path, strerror(errno));
        _exit(1);
    }

    /* Header */
    uint32_t magic = MAGIC;
    uint32_t version = VERSION;
    uint32_t num_levels = 4;
    uint32_t num_keywords = (uint32_t)cfg->num_keywords;
    double total_w = total_weighted;
    double hp_w = hp_score;

    /* Write header as a block */
    uint8_t hdr[4 + 4 + 4 + 4 + 8 + 8];
    memcpy(hdr + 0, &magic, 4);
    memcpy(hdr + 4, &version, 4);
    memcpy(hdr + 8, &num_levels, 4);
    memcpy(hdr + 12, &num_keywords, 4);
    memcpy(hdr + 16, &total_w, 8);
    memcpy(hdr + 24, &hp_w, 8);

    size_t written = fwrite(hdr, 1, sizeof(hdr), bin);
    if (written != sizeof(hdr))
    {
        fprintf(stderr, "Partial write on binary header\n");
        fclose(bin);
        _exit(1);
    }

    /* Write each level_result_t in Region C order: ERROR, WARN, INFO, DEBUG. */
    for (int i = 0; i < 4; i++)
    {
        size_t w = fwrite(&shm_c->results[i], sizeof(level_result_t), 1, bin);
        if (w != 1)
        {
            fprintf(stderr, "Partial write on level_result_t[%d]\n", i);
            fclose(bin);
            _exit(1);
        }
    }
    fclose(bin);

    /* Atomic rename */
    if (rename(tmp_path, cfg->binary_path) != 0)
    {
        fprintf(stderr, "rename failed: %s\n", strerror(errno));
        _exit(1);
    }

    printf("[PID:%d] Output files written: %s, %s\n",
           (int)getpid(), cfg->output_path, cfg->binary_path);
    fflush(stdout);

    printf("[PID:%d] Aggregator exiting.\n", (int)getpid());
    fflush(stdout);
    _exit(0);
}
