#include "stats.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int stats_init(GlobalStats *stats, uint32_t num_couriers)
{
    if (!stats)
        return 0;

    atomic_init(&stats->completed_orders, 0);
    atomic_init(&stats->cancelled_orders, 0);
    atomic_init(&stats->total_delivery_time, 0);

    stats->couriers = malloc(sizeof(CourierStats) * num_couriers);
    if (!stats->couriers)
        return 0;

    for (uint32_t i = 0; i < num_couriers; i++)
    {
        stats->couriers[i].courier_id = i + 1;
        stats->couriers[i].completed = 0;
        stats->couriers[i].total_time = 0;
    }

    stats->num_couriers = num_couriers;
    stats->total_orders = 0;

    return 1;
}

void stats_increment_completed(GlobalStats *stats)
{
    if (stats)
        atomic_fetch_add(&stats->completed_orders, 1);
}

void stats_increment_cancelled(GlobalStats *stats)
{
    if (stats)
        atomic_fetch_add(&stats->cancelled_orders, 1);
}

void stats_add_delivery_time(GlobalStats *stats, uint64_t time_ms)
{
    if (stats)
        atomic_fetch_add(&stats->total_delivery_time, time_ms);
}

void stats_record_courier_completion(GlobalStats *stats, uint32_t courier_id, uint64_t duration_ms)
{
    if (!stats || courier_id < 1 || courier_id > stats->num_couriers)
        return;

    uint32_t idx = courier_id - 1;
    stats->couriers[idx].completed++;
    stats->couriers[idx].total_time += duration_ms;
}

int stats_write_file(const GlobalStats *stats, const char *filename)
{
    if (!stats || !filename)
        return 0;

    FILE *f = fopen(filename, "w");
    if (!f)
        return 0;

    uint32_t completed = atomic_load(&stats->completed_orders);
    uint32_t cancelled = atomic_load(&stats->cancelled_orders);
    uint64_t total_time = atomic_load(&stats->total_delivery_time);

    uint64_t avg_per_order = 0;
    if (completed > 0)
        avg_per_order = total_time / completed;

    /* SHIFT_SUMMARY */
    if (fprintf(f, "SHIFT_SUMMARY\n") < 0)
        goto error;
    if (fprintf(f, "Total orders : %u\n", stats->total_orders) < 0)
        goto error;
    if (fprintf(f, "Completed : %u\n", completed) < 0)
        goto error;
    if (fprintf(f, "Cancelled : %u\n", cancelled) < 0)
        goto error;
    if (fprintf(f, "Total time : %lums\n", (unsigned long)total_time) < 0)
        goto error;
    if (fprintf(f, "Avg per order : %lums\n", (unsigned long)avg_per_order) < 0)
        goto error;

    /* COURIER_STATS */
    if (fprintf(f, "COURIER_STATS\n") < 0)
        goto error;

    for (uint32_t i = 0; i < stats->num_couriers; i++)
    {
        if (fprintf(f, "Courier-%u completed=%u total_time=%lums\n",
                    stats->couriers[i].courier_id,
                    stats->couriers[i].completed,
                    (unsigned long)stats->couriers[i].total_time) < 0)
            goto error;
    }

    fclose(f);
    return 1;

error:
    fclose(f);
    return 0;
}

void stats_free(GlobalStats *stats)
{
    if (stats && stats->couriers)
    {
        free(stats->couriers);
        stats->couriers = NULL;
    }
}
