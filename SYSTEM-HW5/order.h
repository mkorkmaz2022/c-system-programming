#ifndef ORDER_H
#define ORDER_H

#include <stdint.h>

typedef enum
{
    PRIORITY_EXPRESS = 1,
    PRIORITY_STANDARD = 2,
    PRIORITY_ECONOMY = 3
} Priority;

typedef struct
{
    uint32_t id;
    char name[33]; /* max 32 chars + null terminator */
    Priority priority;
    uint32_t duration; /* in simulation units (100ms each) */
} Order;

/* Parse a single line from input file
 * Returns 1 if valid order, 0 if invalid/blank line
 */
int order_parse_line(const char *line, Order *out_order);

/* Convert priority string to enum */
int priority_from_string(const char *str, Priority *out_priority);

/* Convert priority enum to string */
const char *priority_to_string(Priority p);

#endif
