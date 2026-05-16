#define _GNU_SOURCE
#include "analyzer.h"
#include "shm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>

/* Level weights */
static const int level_weights[] = {4, 3, 2, 1};

/* Overlapping keyword count */
static int count_keyword(const char *msg, const char *kw)
{
    int count = 0;
    int klen = (int)strlen(kw);
    if (klen == 0)
        return 0;
    for (int i = 0; msg[i]; i++)
    {
        if (strncmp(msg + i, kw, (size_t)klen) == 0)
            count++;
    }
    return count;
}

/* ---- Shared state across workers in one Analyzer ---- */

/* TLS key (one key for the per-keyword score array) */
static pthread_key_t tls_key;

/* Barrier for workers */
static pthread_barrier_t worker_barrier;

/* Config pointer (set before threads start) */
static const analyzer_config_t *g_cfg = NULL;

/* Tracking minimum TID for reporting thread */
static pid_t min_tid;
static pthread_mutex_t min_tid_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Per-source tracking (shared within analyzer, protected by result_mutex when writing) */
#define MAX_SOURCES_TRACKED 256
typedef struct
{
    char name[MAX_SOURCE];
    long hits;
} source_hit_t;

static source_hit_t source_table[MAX_SOURCES_TRACKED];
static int source_count = 0;
static pthread_mutex_t source_mtx = PTHREAD_MUTEX_INITIALIZER;

static void update_source(const char *src)
{
    pthread_mutex_lock(&source_mtx);
    for (int i = 0; i < source_count; i++)
    {
        if (strcmp(source_table[i].name, src) == 0)
        {
            source_table[i].hits++;
            pthread_mutex_unlock(&source_mtx);
            return;
        }
    }
    if (source_count < MAX_SOURCES_TRACKED)
    {
        strncpy(source_table[source_count].name, src, MAX_SOURCE - 1);
        source_table[source_count].name[MAX_SOURCE - 1] = '\0';
        source_table[source_count].hits = 1;
        source_count++;
    }
    pthread_mutex_unlock(&source_mtx);
}

/* Per-thread score storage (also in Region C per_thread_score) */
static double worker_total_score[MAX_WORKERS];
static int worker_entry_count[MAX_WORKERS];
static pthread_mutex_t scores_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Lets the reporting worker wait until peer TLS destructors have flushed. */
static int tls_active_workers = 0;
static pthread_mutex_t tls_done_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tls_done_cond = PTHREAD_COND_INITIALIZER;

/* ---- TLS destructor: flush per-keyword scores to Region C ---- */
static void tls_destructor(void *val)
{
    if (!val)
        return;
    double *scores = (double *)val;

    pthread_mutex_lock(&shm_c->result_mutex);
    int li = g_cfg->level_index;
    for (int k = 0; k < g_cfg->num_keywords; k++)
    {
        shm_c->results[li].per_keyword_score[k] += scores[k];
    }
    pthread_mutex_unlock(&shm_c->result_mutex);

    free(scores);

    pthread_mutex_lock(&tls_done_mtx);
    tls_active_workers--;
    pthread_cond_broadcast(&tls_done_cond);
    pthread_mutex_unlock(&tls_done_mtx);
}

/* ---- Worker thread ---- */
typedef struct
{
    int level_index;
    int worker_idx;
} worker_arg_t;

static void *worker_thread_func(void *arg)
{
    worker_arg_t *wa = (worker_arg_t *)arg;
    pid_t tid = (pid_t)syscall(SYS_gettid);
    int li = wa->level_index;
    int wi = wa->worker_idx;
    free(wa);

    /* Register TID for minimum tracking */
    pthread_mutex_lock(&min_tid_mtx);
    if (tid < min_tid)
        min_tid = tid;
    pthread_mutex_unlock(&min_tid_mtx);

    printf("[PID:%d][TID:%d] Worker %d started.\n", (int)getpid(), (int)tid, wi);
    fflush(stdout);

    /* Allocate TLS per-keyword scores */
    double *kw_scores = (double *)calloc((size_t)g_cfg->num_keywords, sizeof(double));
    if (!kw_scores)
    {
        perror("calloc kw_scores");
        _exit(1);
    }
    pthread_setspecific(tls_key, kw_scores);

    /* Also store per-thread score in Region C */
    /* (index by wi, written under scores_mtx, flushed after barrier) */

    long entries = 0;
    double w_score = 0.0;
    int weight = level_weights[li];

    while (1)
    {
        /* Consume from Region B[li] */
        pthread_mutex_lock(&shm_b[li]->level_mutex);

        /* Wait until there is data OR eof is posted */
        while (cb_empty(shm_b[li]->head, shm_b[li]->tail) && !shm_b[li]->eof_posted)
            pthread_cond_wait(&shm_b[li]->not_empty_b, &shm_b[li]->level_mutex);

        /* Buffer empty after wake-up means EOF with no more data — stop */
        if (cb_empty(shm_b[li]->head, shm_b[li]->tail))
        {
            pthread_mutex_unlock(&shm_b[li]->level_mutex);
            goto worker_done;
        }

        log_entry_t e = shm_b_buf[li][shm_b[li]->tail];
        shm_b[li]->tail = (shm_b[li]->tail + 1) % shm_b[li]->capacity;
        pthread_cond_signal(&shm_b[li]->not_full_b);
        pthread_mutex_unlock(&shm_b[li]->level_mutex);

        /* Skip EOF marker sentinel entries. */
        if (e.is_eof)
            continue;

        entries++;
        update_source(e.source);

        /* Count keywords */
        for (int k = 0; k < g_cfg->num_keywords; k++)
        {
            int raw = count_keyword(e.message, g_cfg->keywords[k]);
            double ws = (double)raw * (double)weight;
            kw_scores[k] += ws;
            w_score += ws;
        }
    }

worker_done:
    printf("[PID:%d][TID:%d] Worker %d done. Entries: %ld, Weighted score: %.1f\n",
           (int)getpid(), (int)tid, wi, entries, w_score);
    fflush(stdout);

    /* Accumulate total entries and score before barrier */
    pthread_mutex_lock(&scores_mtx);
    worker_entry_count[wi] = (int)entries;
    worker_total_score[wi] = w_score;
    pthread_mutex_unlock(&scores_mtx);

    /* Wait at barrier — destructor flushes TLS AFTER this */
    pthread_barrier_wait(&worker_barrier);

    /* After barrier: TLS destructor not yet called (thread still running).
       We flush per-thread score to Region C now, then let thread exit
       which triggers destructor. */
    pthread_mutex_lock(&shm_c->result_mutex);
    shm_c->results[li].per_thread_score[wi] = w_score;
    shm_c->results[li].total_entries += entries;
    shm_c->results[li].total_weighted_score += w_score;
    pthread_mutex_unlock(&shm_c->result_mutex);

    /* Reporting thread: lowest TID writes final summary */
    pthread_mutex_lock(&min_tid_mtx);
    pid_t local_min = min_tid;
    pthread_mutex_unlock(&min_tid_mtx);

    if (tid != local_min)
        return NULL;

    pthread_mutex_lock(&tls_done_mtx);
    while (tls_active_workers > 1)
        pthread_cond_wait(&tls_done_cond, &tls_done_mtx);
    pthread_mutex_unlock(&tls_done_mtx);

    {
        void *tls_val = pthread_getspecific(tls_key);
        if (tls_val)
        {
            pthread_setspecific(tls_key, NULL);
            tls_destructor(tls_val);
        }
    }

    if (tid == local_min)
    {
        /* Wait briefly for all workers to flush their scores */
        /* (all workers are past barrier, scores_mtx not needed — all done) */

        pthread_mutex_lock(&shm_c->result_mutex);
        long total_entries = shm_c->results[li].total_entries;
        double total_ws = shm_c->results[li].total_weighted_score;

        /* Compute top-3 sources */
        source_hit_t top3[3];
        memset(top3, 0, sizeof(top3));
        for (int s = 0; s < source_count; s++)
        {
            if (source_table[s].hits > top3[0].hits)
            {
                top3[2] = top3[1];
                top3[1] = top3[0];
                top3[0] = source_table[s];
            }
            else if (source_table[s].hits > top3[1].hits)
            {
                top3[2] = top3[1];
                top3[1] = source_table[s];
            }
            else if (source_table[s].hits > top3[2].hits)
            {
                top3[2] = source_table[s];
            }
        }
        for (int i = 0; i < 3; i++)
        {
            strncpy(shm_c->results[li].top_source[i], top3[i].name, MAX_SOURCE - 1);
            shm_c->results[li].top_source[i][MAX_SOURCE - 1] = '\0';
            shm_c->results[li].top_source_hits[i] = top3[i].hits;
        }

        const char *lnames[] = {"ERROR", "WARN", "INFO", "DEBUG"};
        strncpy(shm_c->results[li].level, lnames[li], 7);
        shm_c->results[li].level[7] = '\0';
        shm_c->results[li].ready = 1;
        pthread_cond_broadcast(&shm_c->result_cond);
        pthread_mutex_unlock(&shm_c->result_mutex);

        printf("[PID:%d][TID:%d] ** Reporting thread (lowest TID). Level: %s **\n",
               (int)getpid(), (int)tid, lnames[li]);
        printf("[PID:%d][TID:%d] Total entries: %ld | Total weighted score: %.1f\n",
               (int)getpid(), (int)tid, total_entries, total_ws);
        fflush(stdout);

        /* Post semaphore */
        sem_post(&shm_c->result_sem[li]);
    }

    /* Non-reporting threads exit through the automatic TLS destructor. */
    return NULL;
}

/* ---- Analyzer Process entry ---- */
void run_analyzer(const analyzer_config_t *cfg)
{
    g_cfg = cfg;

    const char *lnames[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    printf("[PID:%d] Analyzer %s started. Workers: %d\n",
           (int)getpid(), lnames[cfg->level_index], cfg->num_workers);
    fflush(stdout);

    /* Initialize shared state */
    min_tid = (pid_t)2147483647; /* INT_MAX */
    source_count = 0;
    memset(source_table, 0, sizeof(source_table));
    memset(worker_total_score, 0, sizeof(worker_total_score));
    memset(worker_entry_count, 0, sizeof(worker_entry_count));
    tls_active_workers = cfg->num_workers;

    /* Initialize Region C result for this level */
    pthread_mutex_lock(&shm_c->result_mutex);
    shm_c->results[cfg->level_index].total_entries = 0;
    shm_c->results[cfg->level_index].total_weighted_score = 0.0;
    memset(shm_c->results[cfg->level_index].per_keyword_score, 0,
           sizeof(shm_c->results[cfg->level_index].per_keyword_score));
    memset(shm_c->results[cfg->level_index].per_thread_score, 0,
           sizeof(shm_c->results[cfg->level_index].per_thread_score));
    pthread_mutex_unlock(&shm_c->result_mutex);

    /* Create TLS key with destructor */
    if (pthread_key_create(&tls_key, tls_destructor) != 0)
    {
        perror("pthread_key_create");
        _exit(1);
    }

    /* Initialize barrier */
    if (pthread_barrier_init(&worker_barrier, NULL, (unsigned)cfg->num_workers) != 0)
    {
        perror("pthread_barrier_init");
        _exit(1);
    }

    /* Spawn worker threads */
    pthread_t *workers = (pthread_t *)malloc((size_t)cfg->num_workers * sizeof(pthread_t));
    if (!workers)
    {
        perror("malloc workers");
        _exit(1);
    }

    for (int i = 0; i < cfg->num_workers; i++)
    {
        worker_arg_t *wa = (worker_arg_t *)malloc(sizeof(worker_arg_t));
        if (!wa)
        {
            perror("malloc wa");
            _exit(1);
        }
        wa->level_index = cfg->level_index;
        wa->worker_idx = i;
        if (pthread_create(&workers[i], NULL, worker_thread_func, wa) != 0)
        {
            perror("pthread_create worker");
            _exit(1);
        }
    }

    for (int i = 0; i < cfg->num_workers; i++)
        pthread_join(workers[i], NULL);

    free(workers);

    pthread_barrier_destroy(&worker_barrier);
    pthread_key_delete(tls_key);
    pthread_mutex_destroy(&min_tid_mtx);
    pthread_mutex_destroy(&source_mtx);
    pthread_mutex_destroy(&scores_mtx);
    pthread_mutex_destroy(&tls_done_mtx);
    pthread_cond_destroy(&tls_done_cond);

    printf("[PID:%d] Analyzer %s exiting.\n", (int)getpid(), lnames[cfg->level_index]);
    fflush(stdout);
    _exit(0);
}
