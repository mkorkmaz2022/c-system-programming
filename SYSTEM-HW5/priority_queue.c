#include "priority_queue.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 100

/* Comparator: returns -1 if a < b, 0 if a == b, 1 if a > b
 * Orders by: priority (ascending), then id (ascending)
 */
static int order_compare(const Order *a, const Order *b)
{
    if (a->priority < b->priority)
        return -1;
    if (a->priority > b->priority)
        return 1;

    if (a->id < b->id)
        return -1;
    if (a->id > b->id)
        return 1;

    return 0;
}

int pq_init(PriorityQueue *pq)
{
    if (!pq)
        return 0;

    pq->capacity = INITIAL_CAPACITY;
    pq->count = 0;
    pq->orders = malloc(sizeof(Order) * pq->capacity);

    if (!pq->orders)
        return 0;

    return 1;
}

int pq_insert(PriorityQueue *pq, const Order *order)
{
    if (!pq || !order)
        return 0;

    /* Grow if necessary */
    if (pq->count >= pq->capacity)
    {
        pq->capacity = pq->capacity * 2;
        Order *new_orders = realloc(pq->orders, sizeof(Order) * pq->capacity);
        if (!new_orders)
            return 0;
        pq->orders = new_orders;
    }

    /* Find the correct position to maintain sorted order */
    uint32_t pos = pq->count;
    for (uint32_t i = 0; i < pq->count; i++)
    {
        if (order_compare(order, &pq->orders[i]) < 0)
        {
            pos = i;
            break;
        }
    }

    /* Shift elements to the right */
    for (uint32_t i = pq->count; i > pos; i--)
    {
        pq->orders[i] = pq->orders[i - 1];
    }

    /* Insert at position */
    pq->orders[pos] = *order;
    pq->count++;

    return 1;
}

int pq_dequeue(PriorityQueue *pq, Order *out_order)
{
    if (!pq || !out_order)
        return 0;

    if (pq->count == 0)
        return 0;

    /* The first element is the highest priority */
    *out_order = pq->orders[0];

    /* Shift all remaining elements left */
    for (uint32_t i = 0; i < pq->count - 1; i++)
    {
        pq->orders[i] = pq->orders[i + 1];
    }

    pq->count--;

    return 1;
}

int pq_is_empty(const PriorityQueue *pq)
{
    if (!pq)
        return 1;
    return pq->count == 0;
}

uint32_t pq_count(const PriorityQueue *pq)
{
    if (!pq)
        return 0;
    return pq->count;
}

void pq_free(PriorityQueue *pq)
{
    if (pq && pq->orders)
    {
        free(pq->orders);
        pq->orders = NULL;
        pq->count = 0;
        pq->capacity = 0;
    }
}
