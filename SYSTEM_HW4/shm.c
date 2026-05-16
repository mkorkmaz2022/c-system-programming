#include "shm.h"
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

region_a_hdr_t *shm_a = NULL;
log_entry_t *shm_a_buf = NULL;

region_b_hdr_t *shm_b[4] = {NULL, NULL, NULL, NULL};
log_entry_t *shm_b_buf[4] = {NULL, NULL, NULL, NULL};

region_c_t *shm_c = NULL;

region_d_hdr_t *shm_d = NULL;
log_entry_t *shm_d_buf = NULL;

size_t shm_a_size = 0;
size_t shm_b_size[4] = {0, 0, 0, 0};
size_t shm_c_size = 0;
size_t shm_d_size = 0;

static void *create_shm(size_t size)
{
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED)
    {
        perror("mmap");
        return NULL;
    }
    memset(ptr, 0, size);
    return ptr;
}

static int init_shared_mutex(pthread_mutex_t *m)
{
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0)
        return -1;
    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0)
    {
        pthread_mutexattr_destroy(&attr);
        return -1;
    }
    int r = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    return r;
}

static int init_shared_cond(pthread_cond_t *c)
{
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0)
        return -1;
    if (pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0)
    {
        pthread_condattr_destroy(&attr);
        return -1;
    }
    int r = pthread_cond_init(c, &attr);
    pthread_condattr_destroy(&attr);
    return r;
}

int shm_init(int cap_a, int cap_b, int cap_d, int total_readers)
{
    /* Region A */
    shm_a_size = sizeof(region_a_hdr_t) + (size_t)cap_a * sizeof(log_entry_t);
    void *ra = create_shm(shm_a_size);
    if (!ra)
        return -1;
    shm_a = (region_a_hdr_t *)ra;
    shm_a_buf = (log_entry_t *)((char *)ra + sizeof(region_a_hdr_t));
    shm_a->capacity = cap_a;
    shm_a->head = 0;
    shm_a->tail = 0;
    shm_a->total_readers = total_readers;
    memset(shm_a->eof_count_per_level, 0, sizeof(shm_a->eof_count_per_level));
    if (init_shared_mutex(&shm_a->input_mutex) != 0)
        return -1;
    if (init_shared_cond(&shm_a->not_full_a) != 0)
        return -1;
    if (init_shared_cond(&shm_a->not_empty_a) != 0)
        return -1;

    /* Region B (4 levels) */
    for (int i = 0; i < 4; i++)
    {
        shm_b_size[i] = sizeof(region_b_hdr_t) + (size_t)cap_b * sizeof(log_entry_t);
        void *rb = create_shm(shm_b_size[i]);
        if (!rb)
            return -1;
        shm_b[i] = (region_b_hdr_t *)rb;
        shm_b_buf[i] = (log_entry_t *)((char *)rb + sizeof(region_b_hdr_t));
        shm_b[i]->capacity = cap_b;
        shm_b[i]->head = 0;
        shm_b[i]->tail = 0;
        shm_b[i]->eof_posted = 0;
        if (init_shared_mutex(&shm_b[i]->level_mutex) != 0)
            return -1;
        if (init_shared_cond(&shm_b[i]->not_full_b) != 0)
            return -1;
        if (init_shared_cond(&shm_b[i]->not_empty_b) != 0)
            return -1;
    }

    /* Region C */
    shm_c_size = sizeof(region_c_t);
    void *rc = create_shm(shm_c_size);
    if (!rc)
        return -1;
    shm_c = (region_c_t *)rc;
    for (int i = 0; i < 4; i++)
    {
        shm_c->results[i].ready = 0;
        if (sem_init(&shm_c->result_sem[i], 1, 0) != 0)
            return -1;
    }
    shm_c->high_priority_score = 0.0;
    if (init_shared_mutex(&shm_c->result_mutex) != 0)
        return -1;
    if (init_shared_cond(&shm_c->result_cond) != 0)
        return -1;

    /* Region D */
    shm_d_size = sizeof(region_d_hdr_t) + (size_t)cap_d * sizeof(log_entry_t);
    void *rd = create_shm(shm_d_size);
    if (!rd)
        return -1;
    shm_d = (region_d_hdr_t *)rd;
    shm_d_buf = (log_entry_t *)((char *)rd + sizeof(region_d_hdr_t));
    shm_d->capacity = cap_d;
    shm_d->head = 0;
    shm_d->tail = 0;
    shm_d->dispatcher_done = 0;
    if (init_shared_mutex(&shm_d->priority_mutex) != 0)
        return -1;
    if (init_shared_cond(&shm_d->not_full_d) != 0)
        return -1;
    if (init_shared_cond(&shm_d->not_empty_d) != 0)
        return -1;

    return 0;
}

void shm_destroy(void)
{
    if (shm_a)
    {
        pthread_mutex_destroy(&shm_a->input_mutex);
        pthread_cond_destroy(&shm_a->not_full_a);
        pthread_cond_destroy(&shm_a->not_empty_a);
        munmap(shm_a, shm_a_size);
        shm_a = NULL;
    }
    for (int i = 0; i < 4; i++)
    {
        if (shm_b[i])
        {
            pthread_mutex_destroy(&shm_b[i]->level_mutex);
            pthread_cond_destroy(&shm_b[i]->not_full_b);
            pthread_cond_destroy(&shm_b[i]->not_empty_b);
            munmap(shm_b[i], shm_b_size[i]);
            shm_b[i] = NULL;
        }
    }
    if (shm_c)
    {
        for (int i = 0; i < 4; i++)
            sem_destroy(&shm_c->result_sem[i]);
        pthread_mutex_destroy(&shm_c->result_mutex);
        pthread_cond_destroy(&shm_c->result_cond);
        munmap(shm_c, shm_c_size);
        shm_c = NULL;
    }
    if (shm_d)
    {
        pthread_mutex_destroy(&shm_d->priority_mutex);
        pthread_cond_destroy(&shm_d->not_full_d);
        pthread_cond_destroy(&shm_d->not_empty_d);
        munmap(shm_d, shm_d_size);
        shm_d = NULL;
    }
}

void shm_force_cleanup(void)
{
    if (shm_c)
    {
        for (int i = 0; i < 4; i++)
            sem_destroy(&shm_c->result_sem[i]);
    }
    if (shm_a)
    {
        munmap(shm_a, shm_a_size);
        shm_a = NULL;
    }
    for (int i = 0; i < 4; i++)
    {
        if (shm_b[i])
        {
            munmap(shm_b[i], shm_b_size[i]);
            shm_b[i] = NULL;
        }
    }
    if (shm_c)
    {
        munmap(shm_c, shm_c_size);
        shm_c = NULL;
    }
    if (shm_d)
    {
        munmap(shm_d, shm_d_size);
        shm_d = NULL;
    }
}
