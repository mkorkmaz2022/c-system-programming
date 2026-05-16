#define _GNU_SOURCE
#include "watchdog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

void *watchdog_thread_func(void *arg)
{
    watchdog_config_t *cfg = (watchdog_config_t *)arg;

    long *reader_lines = (long *)calloc((size_t)cfg->num_readers, sizeof(long));
    if (!reader_lines)
        return NULL;

    time_t start_time = time(NULL);
    char line_buf[256];

    while (!(*cfg->shutdown_flag))
    {
        /* Build fd_set */
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        for (int i = 0; i < cfg->num_readers; i++)
        {
            if (cfg->pipe_read_fds[i] >= 0)
            {
                FD_SET(cfg->pipe_read_fds[i], &rfds);
                if (cfg->pipe_read_fds[i] > maxfd)
                    maxfd = cfg->pipe_read_fds[i];
            }
        }

        struct timeval tv;
        tv.tv_sec = 3;
        tv.tv_usec = 0;

        int sel = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        /* Read available heartbeat lines */
        if (sel > 0)
        {
            for (int i = 0; i < cfg->num_readers; i++)
            {
                if (cfg->pipe_read_fds[i] >= 0 && FD_ISSET(cfg->pipe_read_fds[i], &rfds))
                {
                    /* Non-blocking read of available data */
                    char tmp[1024];
                    ssize_t n = read(cfg->pipe_read_fds[i], tmp, sizeof(tmp) - 1);
                    if (n > 0)
                    {
                        tmp[n] = '\0';
                        /* Parse lines: "[R<i>] <count> lines processed\n" */
                        char *p = tmp;
                        while (*p)
                        {
                            char *nl = strchr(p, '\n');
                            size_t len;
                            if (nl)
                            {
                                len = (size_t)(nl - p);
                            }
                            else
                            {
                                len = strlen(p);
                            }
                            if (len < sizeof(line_buf))
                            {
                                memcpy(line_buf, p, len);
                                line_buf[len] = '\0';
                                /* Parse "[R<i>] <count> lines processed" */
                                int ri = -1;
                                long cnt = 0;
                                if (sscanf(line_buf, "[R%d] %ld lines processed", &ri, &cnt) == 2)
                                {
                                    if (ri >= 0 && ri < cfg->num_readers)
                                        reader_lines[ri] = cnt;
                                }
                            }
                            if (nl)
                                p = nl + 1;
                            else
                                break;
                        }
                    }
                    else if (n == 0)
                    {
                        /* EOF on pipe: reader done */
                        close(cfg->pipe_read_fds[i]);
                        cfg->pipe_read_fds[i] = -1;
                    }
                }
            }
        }

        /* Count alive children */
        int alive = 0;
        for (int i = 0; i < cfg->num_children; i++)
        {
            if (cfg->all_child_pids[i] > 0)
            {
                if (kill(cfg->all_child_pids[i], 0) == 0)
                    alive++;
            }
        }

        long elapsed = (long)(time(NULL) - start_time);
        fprintf(stderr, "[WATCHDOG] Progress at T+%lds:", elapsed);
        for (int i = 0; i < cfg->num_readers; i++)
            fprintf(stderr, " Reader%d=%ld", i, reader_lines[i]);
        fprintf(stderr, " children_alive=%d\n", alive);
        fflush(stderr);
    }

    free(reader_lines);
    return NULL;
}
