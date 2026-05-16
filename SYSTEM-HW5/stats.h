#ifndef STATS_H
#define STATS_H

#include <stdint.h>
#include <stdatomic.h>

typedef struct
{
    uint32_t courier_id;
    uint32_t completed;
    uint64_t total_time;
} CourierStats;

typedef struct
{
    /* Global atomic counters */
    _Atomic(uint32_t) completed_orders;
    _Atomic(uint32_t) cancelled_orders;
    _Atomic(uint64_t) total_delivery_time; /* in milliseconds */

    /* Per-courier stats */
    CourierStats *couriers;
    uint32_t num_couriers;

    /* Total orders read from file */
    uint32_t total_orders;
} GlobalStats;

/* Initialize global stats */
int stats_init(GlobalStats *stats, uint32_t num_couriers);

/* Increment completed orders counter */
void stats_increment_completed(GlobalStats *stats);

/* Increment cancelled orders counter */
void stats_increment_cancelled(GlobalStats *stats);

/* Add delivery time to total */
void stats_add_delivery_time(GlobalStats *stats, uint64_t time_ms);

/* Record a courier's completion */
void stats_record_courier_completion(GlobalStats *stats, uint32_t courier_id, uint64_t duration_ms);

/* Write stats to file */
int stats_write_file(const GlobalStats *stats, const char *filename);

/* Free stats */
void stats_free(GlobalStats *stats);

#endif
