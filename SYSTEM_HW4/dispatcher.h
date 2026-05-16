#ifndef DISPATCHER_H
#define DISPATCHER_H

typedef struct
{
    char **filter_sources;
    int num_filters;
    int total_readers;
    int timeout_sec;
} dispatcher_config_t;

void run_dispatcher(const dispatcher_config_t *cfg);

#endif /* DISPATCHER_H */