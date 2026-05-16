#ifndef READER_H
#define READER_H

/* Configuration for one Reader Process */
typedef struct
{
    int reader_index;   /* which reader (0-based) */
    char filepath[512]; /* log file to read */
    int num_threads;    /* -t flag */
    int pipe_write_fd;  /* write end of heartbeat pipe */
} reader_config_t;

/* Entry point for Reader Process */
void run_reader(const reader_config_t *cfg);

/* Line parser: fills log_entry_t; returns 0 on success, -1 if malformed */
int parse_log_line(const char *line, void *entry_out);

#endif /* READER_H */