#ifndef PRIORITY_QUEUE_H
#define PRIORITY_QUEUE_H

#include "order.h"
#include <stdint.h>

typedef struct
{
    Order *orders;
    uint32_t count;
    uint32_t capacity;
} PriorityQueue;

/* Initialize an empty queue */
int pq_init(PriorityQueue *pq);

/* Insert an order into the queue
 * Orders are kept sorted by priority (ascending), then by id (ascending)
 */
int pq_insert(PriorityQueue *pq, const Order *order);

/* Remove and return the highest-priority order (lowest id on ties)
 * Returns 1 if an order was removed, 0 if queue is empty
 */
int pq_dequeue(PriorityQueue *pq, Order *out_order);

/* Check if queue is empty */
int pq_is_empty(const PriorityQueue *pq);

/* Get current number of orders in queue */
uint32_t pq_count(const PriorityQueue *pq);

/* Free all allocated memory */
void pq_free(PriorityQueue *pq);

#endif
