#define _POSIX_C_SOURCE 200809L
#include "order.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int priority_from_string(const char *str, Priority *out_priority)
{
    if (strcmp(str, "EXPRESS") == 0)
    {
        *out_priority = PRIORITY_EXPRESS;
        return 1;
    }
    else if (strcmp(str, "STANDARD") == 0)
    {
        *out_priority = PRIORITY_STANDARD;
        return 1;
    }
    else if (strcmp(str, "ECONOMY") == 0)
    {
        *out_priority = PRIORITY_ECONOMY;
        return 1;
    }
    return 0;
}

const char *priority_to_string(Priority p)
{
    switch (p)
    {
    case PRIORITY_EXPRESS:
        return "EXPRESS";
    case PRIORITY_STANDARD:
        return "STANDARD";
    case PRIORITY_ECONOMY:
        return "ECONOMY";
    default:
        return "UNKNOWN";
    }
}

/* Check if a string contains only ASCII letters, digits, underscores */
static int is_valid_name(const char *name)
{
    if (!name || strlen(name) == 0 || strlen(name) > 32)
        return 0;

    for (const char *p = name; *p; p++)
    {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_'))
        {
            return 0;
        }
    }
    return 1;
}

/* Check if a string is a valid positive integer */
static int is_valid_uint(const char *str, uint32_t *out_val)
{
    if (!str || strlen(str) == 0)
        return 0;

    for (const char *p = str; *p; p++)
    {
        if (!isdigit((unsigned char)*p))
            return 0;
    }

    errno = 0;
    char *endptr;
    unsigned long val = strtoul(str, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || val == 0 || val > UINT32_MAX)
        return 0;

    *out_val = (uint32_t)val;
    return 1;
}

int order_parse_line(const char *line, Order *out_order)
{
    if (!line || !out_order)
        return 0;

    /* Skip blank lines */
    int has_content = 0;
    for (const char *p = line; *p; p++)
    {
        if (!isspace((unsigned char)*p))
        {
            has_content = 1;
            break;
        }
    }
    if (!has_content)
        return 0;

    /* Make a modifiable copy */
    char buffer[512];
    if (snprintf(buffer, sizeof(buffer), "%s", line) >= (int)sizeof(buffer))
        return 0;

    /* Tokenize, allowing newline and carriage-return delimiters. */
    char *saveptr;
    char *token_id = strtok_r(buffer, " \t\r\n", &saveptr);
    char *token_name = strtok_r(NULL, " \t\r\n", &saveptr);
    char *token_priority = strtok_r(NULL, " \t\r\n", &saveptr);
    char *token_duration = strtok_r(NULL, " \t\r\n", &saveptr);

    if (!token_id || !token_name || !token_priority || !token_duration)
        return 0;

    /* Ensure no extra tokens */
    char *extra = strtok_r(NULL, " \t\r\n", &saveptr);
    if (extra)
        return 0;

    /* Parse id */
    uint32_t id;
    if (!is_valid_uint(token_id, &id))
        return 0;

    /* Parse name */
    if (!is_valid_name(token_name))
        return 0;

    /* Parse priority */
    Priority priority;
    if (!priority_from_string(token_priority, &priority))
        return 0;

    /* Parse duration */
    uint32_t duration;
    if (!is_valid_uint(token_duration, &duration))
        return 0;

    /* Success */
    out_order->id = id;
    strncpy(out_order->name, token_name, 32);
    out_order->name[32] = '\0';
    out_order->priority = priority;
    out_order->duration = duration;

    return 1;
}
