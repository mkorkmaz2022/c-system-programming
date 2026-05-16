#ifndef AGGREGATOR_H
#define AGGREGATOR_H

typedef struct
{
    char **keywords;
    int num_keywords;
    int num_files;
    char output_path[512];
    char binary_path[512];
    char filter_path[512];
    int timeout_sec;
    int num_workers; /* for per-thread output */
} aggregator_config_t;

void run_aggregator(const aggregator_config_t *cfg);

#endif /* AGGREGATOR_H */