#define _GNU_SOURCE
#include "reader.h"
#include "shm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

/* ---- Internal bounded circular buffer (private to one Reader) ---- */
#define INTERNAL_CAP_MULT 4

typedef struct
{
    log_entry_t *buf;
    int capacity;
    int head;
    int tail;
    int done;            /* set when all reader threads finish */
    pthread_mutex_t mtx; /* default (not process-shared) */
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} internal_buf_t;

static internal_buf_t ibuf;

/* ---- Reader thread arguments ---- */
typedef struct
{
    int thread_idx;
    int reader_idx;
    char filepath[512];
    off_t start_byte;
    off_t end_byte; /* -1 = read to EOF */
    int pipe_write_fd;
    int num_reader_threads; /* total T, to know last thread */
} reader_thread_arg_t;

/* ---- Parser thread argument ---- */
typedef struct
{
    int reader_idx;
    int num_reader_threads;
    long counts[4]; /* dispatched per level */
} parser_thread_arg_t;

/* Count of finished reader threads */
static int readers_done_count = 0;
static pthread_mutex_t readers_done_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ---- Parse a single log line ---- */
int parse_log_line(const char *line, void *entry_out)
{
    log_entry_t *e = (log_entry_t *)entry_out;
    /* Format: [YYYY-MM-DD HH:MM:SS] [LEVEL] [SOURCE] MESSAGE */
    const char *p = line;

    memset(e, 0, sizeof(*e));
    e->is_eof = 0;

    /* Skip leading whitespace */
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0')
        return -1; /* blank */

    /* Timestamp: [YYYY-MM-DD HH:MM:SS] */
    if (*p != '[')
        return -1;
    p++;
    const char *ts_start = p;
    while (*p && *p != ']')
        p++;
    if (*p != ']')
        return -1;
    size_t ts_len = (size_t)(p - ts_start);
    if (ts_len >= sizeof(e->timestamp))
        return -1;
    memcpy(e->timestamp, ts_start, ts_len);
    e->timestamp[ts_len] = '\0';
    p++; /* skip ] */

    /* Space */
    if (*p != ' ')
        return -1;
    p++;

    /* Level: [LEVEL] */
    if (*p != '[')
        return -1;
    p++;
    const char *lv_start = p;
    while (*p && *p != ']')
        p++;
    if (*p != ']')
        return -1;
    size_t lv_len = (size_t)(p - lv_start);
    if (lv_len == 0 || lv_len >= sizeof(e->level))
        return -1;
    memcpy(e->level, lv_start, lv_len);
    e->level[lv_len] = '\0';
    p++; /* skip ] */

    /* Validate level */
    if (strcmp(e->level, "ERROR") == 0)
        e->level_index = LEVEL_ERROR;
    else if (strcmp(e->level, "WARN") == 0)
        e->level_index = LEVEL_WARN;
    else if (strcmp(e->level, "INFO") == 0)
        e->level_index = LEVEL_INFO;
    else if (strcmp(e->level, "DEBUG") == 0)
        e->level_index = LEVEL_DEBUG;
    else
        return -1;

    /* Space */
    if (*p != ' ')
        return -1;
    p++;

    /* Source: [SOURCE] */
    if (*p != '[')
        return -1;
    p++;
    const char *src_start = p;
    while (*p && *p != ']')
        p++;
    if (*p != ']')
        return -1;
    size_t src_len = (size_t)(p - src_start);
    if (src_len == 0 || src_len >= sizeof(e->source))
        return -1;
    memcpy(e->source, src_start, src_len);
    e->source[src_len] = '\0';
    /* validate alphanumeric */
    for (size_t i = 0; i < src_len; i++)
    {
        if (!isalnum((unsigned char)e->source[i]))
            return -1;
    }
    p++; /* skip ] */

    /* Message: strip leading whitespace */
    while (*p == ' ' || *p == '\t')
        p++;
    size_t msg_len = strlen(p);
    /* strip trailing newline/cr */
    while (msg_len > 0 && (p[msg_len - 1] == '\n' || p[msg_len - 1] == '\r'))
        msg_len--;
    if (msg_len >= sizeof(e->message))
        msg_len = sizeof(e->message) - 1;
    memcpy(e->message, p, msg_len);
    e->message[msg_len] = '\0';

    return 0;
}

/* Push to internal buffer */
static void ibuf_push(const log_entry_t *e)
{
    pthread_mutex_lock(&ibuf.mtx);
    while (cb_full(ibuf.head, ibuf.tail, ibuf.capacity))
        pthread_cond_wait(&ibuf.not_full, &ibuf.mtx);
    ibuf.buf[ibuf.head] = *e;
    ibuf.head = (ibuf.head + 1) % ibuf.capacity;
    pthread_cond_signal(&ibuf.not_empty);
    pthread_mutex_unlock(&ibuf.mtx);
}

/* ---- Reader thread ---- */
static void *reader_thread_func(void *arg)
{
    reader_thread_arg_t *a = (reader_thread_arg_t *)arg;
    pid_t tid = (pid_t)syscall(SYS_gettid);

    FILE *f = fopen(a->filepath, "r");
    if (!f)
    {
        fprintf(stderr, "[PID:%d][TID:%d] Reader thread %d: cannot open %s: %s\n",
                (int)getpid(), (int)tid, a->thread_idx, a->filepath, strerror(errno));
        /* Signal done */
        pthread_mutex_lock(&readers_done_mtx);
        readers_done_count++;
        pthread_mutex_unlock(&readers_done_mtx);
        pthread_mutex_lock(&ibuf.mtx);
        ibuf.done++;
        pthread_cond_signal(&ibuf.not_empty);
        pthread_mutex_unlock(&ibuf.mtx);
        free(a);
        return NULL;
    }

    /* Seek to start */
    off_t pos = a->start_byte;
    if (pos > 0)
    {
        if (fseeko(f, pos, SEEK_SET) != 0)
        {
            fclose(f);
            pthread_mutex_lock(&readers_done_mtx);
            readers_done_count++;
            pthread_mutex_unlock(&readers_done_mtx);
            pthread_mutex_lock(&ibuf.mtx);
            ibuf.done++;
            pthread_cond_signal(&ibuf.not_empty);
            pthread_mutex_unlock(&ibuf.mtx);
            free(a);
            return NULL;
        }
        /* skip partial line at boundary (not thread 0) */
        if (a->thread_idx > 0)
        {
            char discard[4096];
            if (fgets(discard, sizeof(discard), f) == NULL)
            {
                /* empty or error - still ok */
            }
        }
    }

    long lines_read = 0;
    long malformed = 0;
    long heartbeat_ctr = 0;
    char line[4096];
    log_entry_t entry;

    printf("[PID:%d][TID:%d] Reader thread %d: range [%lld, %lld) bytes\n",
           (int)getpid(), (int)tid, a->thread_idx,
           (long long)a->start_byte, (long long)(a->end_byte < 0 ? -1 : a->end_byte));
    fflush(stdout);

    while (1)
    {
        off_t cur = ftello(f);
        if (a->end_byte >= 0 && cur >= a->end_byte)
            break;
        if (fgets(line, sizeof(line), f) == NULL)
            break;

        if (parse_log_line(line, &entry) == 0)
        {
            ibuf_push(&entry);
            lines_read++;
        }
        else
        {
            /* skip blank lines silently */
            char *tmp = line;
            while (*tmp == ' ' || *tmp == '\t' || *tmp == '\r' || *tmp == '\n')
                tmp++;
            if (*tmp != '\0')
                malformed++;
        }

        heartbeat_ctr++;
        if (heartbeat_ctr % 50 == 0)
        {
            char hb[128];
            int hlen = snprintf(hb, sizeof(hb), "[R%d] %ld lines processed\n",
                                a->reader_idx, lines_read);
            if (hlen > 0)
                (void)write(a->pipe_write_fd, hb, (size_t)hlen);
        }
    }
    fclose(f);

    /* Final heartbeat */
    char hb[128];
    int hlen = snprintf(hb, sizeof(hb), "[R%d] %ld lines processed\n",
                        a->reader_idx, lines_read);
    if (hlen > 0)
        (void)write(a->pipe_write_fd, hb, (size_t)hlen);

    printf("[PID:%d][TID:%d] Reader thread %d: finished, lines_read=%ld, malformed=%ld\n",
           (int)getpid(), (int)tid, a->thread_idx, lines_read, malformed);
    fflush(stdout);

    pthread_mutex_lock(&readers_done_mtx);
    readers_done_count++;
    pthread_mutex_unlock(&readers_done_mtx);

    pthread_mutex_lock(&ibuf.mtx);
    ibuf.done++;
    pthread_cond_signal(&ibuf.not_empty);
    pthread_mutex_unlock(&ibuf.mtx);

    free(a);
    return NULL;
}

/* Push entry to Region A */
static void push_to_region_a(const log_entry_t *e)
{
    pthread_mutex_lock(&shm_a->input_mutex);
    while (cb_full(shm_a->head, shm_a->tail, shm_a->capacity))
        pthread_cond_wait(&shm_a->not_full_a, &shm_a->input_mutex);
    shm_a_buf[shm_a->head] = *e;
    shm_a->head = (shm_a->head + 1) % shm_a->capacity;
    pthread_cond_signal(&shm_a->not_empty_a);
    pthread_mutex_unlock(&shm_a->input_mutex);
}

/* ---- Parser thread ---- */
static void *parser_thread_func(void *arg)
{
    parser_thread_arg_t *pa = (parser_thread_arg_t *)arg;
    int total_done = 0;
    long counts[4] = {0, 0, 0, 0};

    while (1)
    {
        pthread_mutex_lock(&ibuf.mtx);
        while (cb_empty(ibuf.head, ibuf.tail) && ibuf.done < pa->num_reader_threads)
            pthread_cond_wait(&ibuf.not_empty, &ibuf.mtx);

        if (cb_empty(ibuf.head, ibuf.tail) && ibuf.done >= pa->num_reader_threads)
        {
            total_done = ibuf.done;
            (void)total_done;
            pthread_mutex_unlock(&ibuf.mtx);
            break;
        }

        log_entry_t e = ibuf.buf[ibuf.tail];
        ibuf.tail = (ibuf.tail + 1) % ibuf.capacity;
        pthread_cond_signal(&ibuf.not_full);
        pthread_mutex_unlock(&ibuf.mtx);

        push_to_region_a(&e);
        counts[e.level_index]++;
    }

    /* Send 4 EOF markers to Region A (one per level) */
    for (int l = 0; l < 4; l++)
    {
        log_entry_t eof;
        memset(&eof, 0, sizeof(eof));
        eof.is_eof = 1;
        eof.level_index = l;

        pthread_mutex_lock(&shm_a->input_mutex);
        while (cb_full(shm_a->head, shm_a->tail, shm_a->capacity))
            pthread_cond_wait(&shm_a->not_full_a, &shm_a->input_mutex);
        shm_a_buf[shm_a->head] = eof;
        shm_a->head = (shm_a->head + 1) % shm_a->capacity;
        shm_a->eof_count_per_level[l]++;
        pthread_cond_signal(&shm_a->not_empty_a);
        pthread_mutex_unlock(&shm_a->input_mutex);
    }

    printf("[PID:%d] Parser thread: dispatched E:%ld W:%ld I:%ld D:%ld -> Region A\n",
           (int)getpid(), counts[0], counts[1], counts[2], counts[3]);
    fflush(stdout);

    memcpy(pa->counts, counts, sizeof(counts));
    return NULL;
}

/* ---- Main Reader Process entry ---- */
void run_reader(const reader_config_t *cfg)
{
    printf("[PID:%d] Reader %d started. File: %s, Threads: %d\n",
           (int)getpid(), cfg->reader_index, cfg->filepath, cfg->num_threads);
    fflush(stdout);

    /* Get file size */
    FILE *f = fopen(cfg->filepath, "r");
    if (!f)
    {
        fprintf(stderr, "[PID:%d] Reader %d: cannot open %s: %s\n",
                (int)getpid(), cfg->reader_index, cfg->filepath, strerror(errno));
        _exit(1);
    }
    if (fseeko(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        _exit(1);
    }
    off_t file_size = ftello(f);
    fclose(f);

    /* Initialize internal buffer */
    int ibuf_cap = cfg->num_threads * INTERNAL_CAP_MULT;
    if (ibuf_cap < 4)
        ibuf_cap = 4;
    ibuf.buf = (log_entry_t *)malloc((size_t)ibuf_cap * sizeof(log_entry_t));
    if (!ibuf.buf)
    {
        perror("malloc ibuf");
        _exit(1);
    }
    ibuf.capacity = ibuf_cap;
    ibuf.head = 0;
    ibuf.tail = 0;
    ibuf.done = 0;
    pthread_mutex_init(&ibuf.mtx, NULL);
    pthread_cond_init(&ibuf.not_full, NULL);
    pthread_cond_init(&ibuf.not_empty, NULL);
    readers_done_count = 0;

    int T = cfg->num_threads;
    pthread_t *reader_threads = (pthread_t *)malloc((size_t)T * sizeof(pthread_t));
    if (!reader_threads)
    {
        perror("malloc threads");
        _exit(1);
    }

    off_t base = (file_size > 0) ? (file_size / T) : 0;

    for (int i = 0; i < T; i++)
    {
        reader_thread_arg_t *a = (reader_thread_arg_t *)malloc(sizeof(reader_thread_arg_t));
        if (!a)
        {
            perror("malloc arg");
            _exit(1);
        }
        a->thread_idx = i;
        a->reader_idx = cfg->reader_index;
        a->pipe_write_fd = cfg->pipe_write_fd;
        a->num_reader_threads = T;
        strncpy(a->filepath, cfg->filepath, sizeof(a->filepath) - 1);
        a->filepath[sizeof(a->filepath) - 1] = '\0';

        a->start_byte = (off_t)i * base;
        if (i == T - 1)
            a->end_byte = -1; /* last thread reads to EOF */
        else
            a->end_byte = (off_t)(i + 1) * base;

        if (pthread_create(&reader_threads[i], NULL, reader_thread_func, a) != 0)
        {
            perror("pthread_create reader thread");
            _exit(1);
        }
    }

    /* Start parser thread */
    parser_thread_arg_t pa;
    pa.reader_idx = cfg->reader_index;
    pa.num_reader_threads = T;
    memset(pa.counts, 0, sizeof(pa.counts));

    pthread_t parser_tid;
    if (pthread_create(&parser_tid, NULL, parser_thread_func, &pa) != 0)
    {
        perror("pthread_create parser");
        _exit(1);
    }

    /* Join reader threads */
    for (int i = 0; i < T; i++)
        pthread_join(reader_threads[i], NULL);

    /* Join parser thread */
    pthread_join(parser_tid, NULL);

    free(reader_threads);
    free(ibuf.buf);
    pthread_mutex_destroy(&ibuf.mtx);
    pthread_cond_destroy(&ibuf.not_full);
    pthread_cond_destroy(&ibuf.not_empty);
    pthread_mutex_destroy(&readers_done_mtx);

    close(cfg->pipe_write_fd);

    printf("[PID:%d] Reader %d exiting.\n", (int)getpid(), cfg->reader_index);
    fflush(stdout);
    _exit(0);
}
