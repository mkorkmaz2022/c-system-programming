#ifndef SHM_H
#define SHM_H

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>

#define MAX_KEYWORDS 8
#define MAX_WORKERS 64
#define MAX_SOURCE 64
#define MAX_MSG 512
#define MAX_LEVEL 8
#define MAX_FILES 256
#define MAX_FILTERS 256

/* Log entry structure */
typedef struct
{
    char timestamp[32];
    char level[8];
    char source[MAX_SOURCE];
    char message[MAX_MSG];
    int level_index; /* 0=ERROR,1=WARN,2=INFO,3=DEBUG */
    int is_eof;      /* 1 for EOF sentinel entries, 0 for normal log entries */
} log_entry_t;

/* Level indices */
#define LEVEL_ERROR 0
#define LEVEL_WARN 1
#define LEVEL_INFO 2
#define LEVEL_DEBUG 3

/* ---- Region A: Dispatcher Input Queue ---- */
typedef struct
{
    pthread_mutex_t input_mutex;
    pthread_cond_t not_full_a;
    pthread_cond_t not_empty_a;
    int eof_count_per_level[4];
    int total_readers;
    int head;
    int tail;
    int capacity;
    /* buffer follows in memory */
} region_a_hdr_t;

/* ---- Region B: Per-Level Analysis Buffers ---- */
typedef struct
{
    pthread_mutex_t level_mutex;
    pthread_cond_t not_full_b;
    pthread_cond_t not_empty_b;
    int eof_posted;
    int head;
    int tail;
    int capacity;
    /* buffer follows in memory */
} region_b_hdr_t;

/* ---- Region C: Results Area ---- */
typedef struct
{
    char level[MAX_LEVEL];
    long total_entries;
    double total_weighted_score;
    double per_keyword_score[MAX_KEYWORDS];
    double per_thread_score[MAX_WORKERS];
    char top_source[3][MAX_SOURCE];
    long top_source_hits[3];
    int ready;
} level_result_t;

typedef struct
{
    level_result_t results[4];
    sem_t result_sem[4];
    pthread_mutex_t result_mutex;
    pthread_cond_t result_cond;
    double high_priority_score;
} region_c_t;

/* ---- Region D: High-Priority Buffer ---- */
typedef struct
{
    pthread_mutex_t priority_mutex;
    pthread_cond_t not_full_d;
    pthread_cond_t not_empty_d;
    int dispatcher_done;
    int head;
    int tail;
    int capacity;
    /* buffer follows in memory */
} region_d_hdr_t;

/* Pointers to shared regions (set by parent, inherited by children) */
extern region_a_hdr_t *shm_a;
extern log_entry_t *shm_a_buf;

extern region_b_hdr_t *shm_b[4];
extern log_entry_t *shm_b_buf[4];

extern region_c_t *shm_c;

extern region_d_hdr_t *shm_d;
extern log_entry_t *shm_d_buf;

/* Sizes for munmap */
extern size_t shm_a_size;
extern size_t shm_b_size[4];
extern size_t shm_c_size;
extern size_t shm_d_size;

/* Initialize all shared memory regions */
int shm_init(int cap_a, int cap_b, int cap_d, int total_readers);
void shm_destroy(void);
void shm_force_cleanup(void);

/* Circular buffer helpers (inline) */
static inline int cb_full(int head, int tail, int cap)
{
    return ((head + 1) % cap) == tail;
}
static inline int cb_empty(int head, int tail)
{
    return head == tail;
}

#endif /* SHM_H */
