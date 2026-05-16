#ifndef ANALYZER_H
#define ANALYZER_H

typedef struct
{
    int level_index; /* 0=ERROR,1=WARN,2=INFO,3=DEBUG */
    int num_workers; /* -w flag */
    char **keywords;
    int num_keywords;
    int timeout_sec;
} analyzer_config_t;

void run_analyzer(const analyzer_config_t *cfg);

#endif /* ANALYZER_H */