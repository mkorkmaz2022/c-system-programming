#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include "order.h"
#include "priority_queue.h"
#include "courier.h"
#include "stats.h"

/* Global state for SIGINT handling */
static _Atomic(int) shutdown_flag = 0;
static _Atomic(uint32_t) active_deliveries = 0;
static pthread_t *courier_threads = NULL;
static pthread_t signal_thread;
static uint32_t num_couriers = 0;
static PriorityQueue *queue = NULL;
static pthread_mutex_t *queue_mutex = NULL;
static pthread_cond_t *queue_cond = NULL;
static pthread_cond_t *done_cond = NULL;
static pthread_mutex_t *log_mutex = NULL;
static GlobalStats *stats = NULL;

static void cleanup_shutdown(void);

/* Signal thread: waits for SIGINT and performs shutdown cleanup */
static void *signal_thread_func(void *arg)
{
    (void)arg;
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);

    while (!atomic_load(&shutdown_flag))
    {
        struct timespec timeout = {.tv_sec = 0, .tv_nsec = 100000000};
        int signo = sigtimedwait(&sigset, NULL, &timeout);
        if (signo == SIGINT)
        {
            atomic_store(&shutdown_flag, 1);
            cleanup_shutdown();
            break;
        }
        else if (signo == -1 && errno == EAGAIN)
        {
            continue;
        }
        else if (signo == -1)
        {
            break;
        }
    }

    return NULL;
}

/* Read orders from file */
static int load_orders_from_file(const char *filename, PriorityQueue *pq, GlobalStats *gstats)
{
    if (!filename || !pq || !gstats)
        return 0;

    FILE *f = fopen(filename, "r");
    if (!f)
    {
        fprintf(stderr, "Error: cannot open file '%s'\n", filename);
        return 0;
    }

    char line[512];
    int order_count = 0;

    while (fgets(line, sizeof(line), f))
    {
        Order order;
        if (order_parse_line(line, &order))
        {
            if (!pq_insert(pq, &order))
            {
                fprintf(stderr, "Error: cannot insert order into queue\n");
                fclose(f);
                return 0;
            }
            order_count++;
        }
        /* Silently skip invalid lines */
    }

    fclose(f);
    gstats->total_orders = order_count;
    return 1;
}

/* Print usage message */
static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s -n <num_couriers> -i <orders.txt> -s <stats.txt>\n", prog);
    fprintf(stderr, "  -n <num_couriers>  Number of courier threads (>= 1)\n");
    fprintf(stderr, "  -i <orders.txt>    Input file with orders\n");
    fprintf(stderr, "  -s <stats.txt>     Output statistics file\n");
}

/* Parse command-line arguments */
static int parse_args(int argc, char *argv[],
                      uint32_t *out_num_couriers,
                      const char **out_input_file,
                      const char **out_stats_file)
{
    if (argc < 7)
        return 0;

    *out_num_couriers = 0;
    *out_input_file = NULL;
    *out_stats_file = NULL;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-n") == 0)
        {
            if (i + 1 >= argc)
                return 0;
            char *endptr;
            long val = strtol(argv[++i], &endptr, 10);
            if (val < 1 || *endptr != '\0')
                return 0;
            *out_num_couriers = (uint32_t)val;
        }
        else if (strcmp(argv[i], "-i") == 0)
        {
            if (i + 1 >= argc)
                return 0;
            *out_input_file = argv[++i];
        }
        else if (strcmp(argv[i], "-s") == 0)
        {
            if (i + 1 >= argc)
                return 0;
            *out_stats_file = argv[++i];
        }
        else
        {
            return 0;
        }
    }

    return (*out_num_couriers > 0 && *out_input_file && *out_stats_file);
}

/* Cleanup function called by SIGINT or at end */
static void cleanup_shutdown(void)
{
    if (!queue || !queue_mutex || !queue_cond || !log_mutex)
        return;

    int pending_orders = 0;

    /* Lock queue, count pending, cancel all */
    pthread_mutex_lock(queue_mutex);

    pending_orders = pq_count(queue);

    /* Log SIGINT_RECEIVED */
    pthread_mutex_lock(log_mutex);
    printf("[CARGOGTU] SIGINT_RECEIVED pending_orders=%d\n", pending_orders);
    pthread_mutex_unlock(log_mutex);

    /* Cancel all pending orders */
    Order order;
    while (pq_dequeue(queue, &order))
    {
        pthread_mutex_lock(log_mutex);
        printf("[CARGOGTU] ORDER_CANCELLED id=%u recipient=%s priority=%s\n",
               order.id, order.name, priority_to_string(order.priority));
        pthread_mutex_unlock(log_mutex);

        stats_increment_cancelled(stats);
    }

    /* Broadcast to wake up waiting couriers and main termination wait */
    pthread_cond_broadcast(queue_cond);
    pthread_cond_broadcast(done_cond);

    pthread_mutex_unlock(queue_mutex);
}

int main(int argc, char *argv[])
{
    uint32_t num_couriers_val;
    const char *input_file;
    const char *stats_file;

    /* Parse arguments */
    if (!parse_args(argc, argv, &num_couriers_val, &input_file, &stats_file))
    {
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    /* Initialize structures */
    queue = malloc(sizeof(PriorityQueue));
    queue_mutex = malloc(sizeof(pthread_mutex_t));
    queue_cond = malloc(sizeof(pthread_cond_t));
    log_mutex = malloc(sizeof(pthread_mutex_t));
    stats = malloc(sizeof(GlobalStats));
    courier_threads = malloc(sizeof(pthread_t) * num_couriers_val);

    if (!queue || !queue_mutex || !queue_cond || !log_mutex || !stats || !courier_threads)
    {
        fprintf(stderr, "Error: memory allocation failed\n");
        exit(EXIT_FAILURE);
    }

    /* Initialize queue and synchronization primitives */
    if (!pq_init(queue))
    {
        fprintf(stderr, "Error: cannot initialize priority queue\n");
        exit(EXIT_FAILURE);
    }

    if (pthread_mutex_init(queue_mutex, NULL) != 0)
    {
        fprintf(stderr, "Error: pthread_mutex_init failed\n");
        exit(EXIT_FAILURE);
    }

    if (pthread_cond_init(queue_cond, NULL) != 0)
    {
        fprintf(stderr, "Error: pthread_cond_init failed\n");
        exit(EXIT_FAILURE);
    }

    done_cond = malloc(sizeof(pthread_cond_t));
    if (!done_cond)
    {
        fprintf(stderr, "Error: memory allocation failed\n");
        exit(EXIT_FAILURE);
    }
    if (pthread_cond_init(done_cond, NULL) != 0)
    {
        fprintf(stderr, "Error: pthread_cond_init failed\n");
        exit(EXIT_FAILURE);
    }

    if (pthread_mutex_init(log_mutex, NULL) != 0)
    {
        fprintf(stderr, "Error: pthread_mutex_init failed\n");
        exit(EXIT_FAILURE);
    }

    if (!stats_init(stats, num_couriers_val))
    {
        fprintf(stderr, "Error: cannot initialize stats\n");
        exit(EXIT_FAILURE);
    }

    num_couriers = num_couriers_val;

    /* Load orders from file */
    if (!load_orders_from_file(input_file, queue, stats))
    {
        fprintf(stderr, "Error: cannot load orders from file\n");
        exit(EXIT_FAILURE);
    }

    /* Block SIGINT in this thread and all subsequently created threads */
    sigset_t block_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGINT);
    if (pthread_sigmask(SIG_BLOCK, &block_set, NULL) != 0)
    {
        fprintf(stderr, "Error: pthread_sigmask failed\n");
        exit(EXIT_FAILURE);
    }

    /* Create dedicated signal thread */
    if (pthread_create(&signal_thread, NULL, signal_thread_func, NULL) != 0)
    {
        fprintf(stderr, "Error: pthread_create failed for signal thread\n");
        exit(EXIT_FAILURE);
    }

    /* Log SHIFT_START */
    pthread_mutex_lock(log_mutex);
    printf("[CARGOGTU] SHIFT_START couriers=%u orders=%u\n", num_couriers, stats->total_orders);
    pthread_mutex_unlock(log_mutex);

    /* Log all queued orders */
    for (uint32_t i = 0; i < pq_count(queue); i++)
    {
        pthread_mutex_lock(log_mutex);
        printf("[CARGOGTU] ORDER_QUEUED id=%u recipient=%s priority=%s duration=%u\n",
               queue->orders[i].id, queue->orders[i].name,
               priority_to_string(queue->orders[i].priority),
               queue->orders[i].duration);
        pthread_mutex_unlock(log_mutex);
    }

    /* Create courier threads */
    for (uint32_t i = 0; i < num_couriers; i++)
    {
        CourierContext *ctx = malloc(sizeof(CourierContext));
        if (!ctx)
        {
            fprintf(stderr, "Error: memory allocation failed\n");
            exit(EXIT_FAILURE);
        }

        ctx->queue = queue;
        ctx->queue_mutex = queue_mutex;
        ctx->queue_cond = queue_cond;
        ctx->log_mutex = log_mutex;
        ctx->stats = stats;
        ctx->courier_id = i + 1;
        ctx->shutdown_flag = &shutdown_flag;
        ctx->active_deliveries = &active_deliveries;
        ctx->done_cond = done_cond;

        if (pthread_create(&courier_threads[i], NULL, courier_thread_func, ctx) != 0)
        {
            fprintf(stderr, "Error: pthread_create failed\n");
            exit(EXIT_FAILURE);
        }
    }

    /* Main thread: wait for all orders to finish or SIGINT shutdown */
    pthread_mutex_lock(queue_mutex);
    while (!atomic_load(&shutdown_flag) &&
           (pq_count(queue) > 0 || atomic_load(&active_deliveries) > 0))
    {
        pthread_cond_wait(done_cond, queue_mutex);
    }
    pthread_mutex_unlock(queue_mutex);

    /* Set shutdown flag to tell couriers to exit */
    atomic_store(&shutdown_flag, 1);

    /* Wake up all waiting couriers */
    pthread_mutex_lock(queue_mutex);
    pthread_cond_broadcast(queue_cond);
    pthread_cond_broadcast(done_cond);
    pthread_mutex_unlock(queue_mutex);

    /* Join all courier threads */
    for (uint32_t i = 0; i < num_couriers; i++)
    {
        pthread_join(courier_threads[i], NULL);
    }

    /* Join signal thread */
    pthread_join(signal_thread, NULL);

    /* Get final counters */
    uint32_t completed = atomic_load(&stats->completed_orders);
    uint32_t cancelled = atomic_load(&stats->cancelled_orders);
    uint64_t total_time = atomic_load(&stats->total_delivery_time);

    /* Log SHIFT_END */
    pthread_mutex_lock(log_mutex);
    printf("[CARGOGTU] SHIFT_END completed=%u cancelled=%u total_time=%lums\n",
           completed, cancelled, (unsigned long)total_time);
    pthread_mutex_unlock(log_mutex);

    /* Write stats file */
    if (!stats_write_file(stats, stats_file))
    {
        fprintf(stderr, "Error: cannot write stats file '%s'\n", stats_file);
        exit(EXIT_FAILURE);
    }

    /* Log SHUTDOWN_COMPLETE */
    pthread_mutex_lock(log_mutex);
    printf("[CARGOGTU] SHUTDOWN_COMPLETE\n");
    pthread_mutex_unlock(log_mutex);

    /* Cleanup */
    pq_free(queue);
    pthread_mutex_destroy(queue_mutex);
    pthread_cond_destroy(queue_cond);
    pthread_cond_destroy(done_cond);
    pthread_mutex_destroy(log_mutex);
    stats_free(stats);

    free(queue);
    free(queue_mutex);
    free(queue_cond);
    free(done_cond);
    free(log_mutex);
    free(stats);
    free(courier_threads);

    return EXIT_SUCCESS;
}
