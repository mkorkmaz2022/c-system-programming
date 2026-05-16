#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include "shm.h"
#include "reader.h"
#include "dispatcher.h"
#include "analyzer.h"
#include "aggregator.h"
#include "watchdog.h"

/* ---- Global volatile flags ---- */
static volatile sig_atomic_t sigint_received = 0;
static volatile sig_atomic_t shutdown_watchdog = 0;

/* ---- Child PID storage ---- */
#define MAX_CHILDREN 512
static pid_t child_pids[MAX_CHILDREN];
static int num_children = 0;

/* ---- SIGINT handler (async-signal-safe) ---- */
static void sigint_handler(int sig)
{
    (void)sig;
    sigint_received = 1;
    /* Async-signal-safe message */
    const char msg[] = "[SIGINT received]\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
}

/* ---- Usage ---- */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -c <config> -f <filter> -k <keywords> -t <threads> -w <workers>"
            " -a <capA> -b <capB> -d <capD> [-T <timeout>] -o <output> -O <binary>\n",
            prog);
}

/* ---- Read lines from file into array ---- */
static char **read_lines(const char *path, int *count)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        fprintf(stderr, "Cannot open file '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    char **lines = NULL;
    int cap = 0;
    int n = 0;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f))
    {
        /* Strip newline */
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        {
            buf[--len] = '\0';
        }
        if (len == 0)
            continue;
        if (n >= cap)
        {
            cap = cap ? cap * 2 : 16;
            char **tmp = (char **)realloc(lines, (size_t)cap * sizeof(char *));
            if (!tmp)
            {
                fclose(f);
                free(lines);
                return NULL;
            }
            lines = tmp;
        }
        lines[n] = strdup(buf);
        if (!lines[n])
        {
            fclose(f);
            return NULL;
        }
        n++;
    }
    fclose(f);
    *count = n;
    return lines;
}

/* ---- Split keywords by comma ---- */
static char **split_keywords(const char *kw_str, int *count)
{
    char *dup = strdup(kw_str);
    if (!dup)
        return NULL;
    char **arr = (char **)malloc(MAX_KEYWORDS * sizeof(char *));
    if (!arr)
    {
        free(dup);
        return NULL;
    }
    int n = 0;
    char *tok = strtok(dup, ",");
    while (tok)
    {
        if (n >= MAX_KEYWORDS || tok[0] == '\0' || strpbrk(tok, " \t\r\n") != NULL)
        {
            for (int i = 0; i < n; i++)
                free(arr[i]);
            free(arr);
            free(dup);
            *count = 0;
            return NULL;
        }
        arr[n++] = strdup(tok);
        if (!arr[n - 1])
        {
            for (int i = 0; i < n - 1; i++)
                free(arr[i]);
            free(arr);
            free(dup);
            *count = 0;
            return NULL;
        }
        tok = strtok(NULL, ",");
    }
    free(dup);
    *count = n;
    return arr;
}

int main(int argc, char *argv[])
{
    /* ---- Parse arguments ---- */
    char *config_file = NULL;
    char *filter_file = NULL;
    char *keywords_str = NULL;
    int num_threads = 0;
    int num_workers = 0;
    int cap_a = 0;
    int cap_b = 0;
    int cap_d = 0;
    int timeout_sec = 10;
    char *output_path = NULL;
    char *binary_path = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "c:f:k:t:w:a:b:d:T:o:O:")) != -1)
    {
        switch (opt)
        {
        case 'c':
            config_file = optarg;
            break;
        case 'f':
            filter_file = optarg;
            break;
        case 'k':
            keywords_str = optarg;
            break;
        case 't':
            num_threads = atoi(optarg);
            break;
        case 'w':
            num_workers = atoi(optarg);
            break;
        case 'a':
            cap_a = atoi(optarg);
            break;
        case 'b':
            cap_b = atoi(optarg);
            break;
        case 'd':
            cap_d = atoi(optarg);
            break;
        case 'T':
            timeout_sec = atoi(optarg);
            break;
        case 'o':
            output_path = optarg;
            break;
        case 'O':
            binary_path = optarg;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* Validate required args */
    int err = 0;
    if (!config_file)
    {
        fprintf(stderr, "Missing -c\n");
        err = 1;
    }
    if (!filter_file)
    {
        fprintf(stderr, "Missing -f\n");
        err = 1;
    }
    if (!keywords_str)
    {
        fprintf(stderr, "Missing -k\n");
        err = 1;
    }
    if (!output_path)
    {
        fprintf(stderr, "Missing -o\n");
        err = 1;
    }
    if (!binary_path)
    {
        fprintf(stderr, "Missing -O\n");
        err = 1;
    }
    if (num_threads < 1)
    {
        fprintf(stderr, "Invalid -t (must be >= 1)\n");
        err = 1;
    }
    if (num_workers < 1 || num_workers > MAX_WORKERS)
    {
        fprintf(stderr, "Invalid -w (1-64)\n");
        err = 1;
    }
    if (cap_a < 4)
    {
        fprintf(stderr, "Invalid -a (must be >= 4)\n");
        err = 1;
    }
    if (cap_b < 4)
    {
        fprintf(stderr, "Invalid -b (must be >= 4)\n");
        err = 1;
    }
    if (cap_d < 2)
    {
        fprintf(stderr, "Invalid -d (must be >= 2)\n");
        err = 1;
    }
    if (timeout_sec < 1)
    {
        fprintf(stderr, "Invalid -T (must be >= 1)\n");
        err = 1;
    }
    if (err)
    {
        usage(argv[0]);
        return 1;
    }

    /* Read config and filter files */
    int num_files = 0;
    char **log_files = read_lines(config_file, &num_files);
    if (!log_files || num_files == 0)
    {
        fprintf(stderr, "No log files in config file '%s'\n", config_file);
        return 1;
    }

    int num_filters = 0;
    char **filter_srcs = read_lines(filter_file, &num_filters);
    if (!filter_srcs)
        filter_srcs = NULL; /* empty filter is OK */

    int num_keywords = 0;
    char **keywords = split_keywords(keywords_str, &num_keywords);
    if (!keywords || num_keywords == 0)
    {
        fprintf(stderr, "No valid keywords in '%s'\n", keywords_str);
        return 1;
    }
    if (num_keywords > MAX_KEYWORDS)
    {
        fprintf(stderr, "Too many keywords (max %d)\n", MAX_KEYWORDS);
        return 1;
    }

    printf("[PID:%d] Parent started. Files: %d, Keywords: %s\n",
           (int)getpid(), num_files, keywords_str);
    fflush(stdout);

    /* ---- Initialize shared memory ---- */
    if (shm_init(cap_a, cap_b, cap_d, num_files) != 0)
    {
        fprintf(stderr, "shm_init failed\n");
        return 1;
    }
    printf("[PID:%d] Shared memory initialized (A:%d B:%dx4 D:%d).\n",
           (int)getpid(), cap_a, cap_b, cap_d);
    fflush(stdout);

    /* ---- Install SIGINT handler ---- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);

    /* ---- Create pipes for Reader heartbeats ---- */
    int (*pipe_fds)[2] = (int (*)[2])malloc((size_t)num_files * 2 * sizeof(int));
    if (!pipe_fds)
    {
        perror("malloc pipe_fds");
        return 1;
    }
    for (int i = 0; i < num_files; i++)
    {
        if (pipe(pipe_fds[i]) != 0)
        {
            perror("pipe");
            return 1;
        }
    }

    /* ---- Fork Reader Processes ---- */
    for (int i = 0; i < num_files; i++)
    {
        printf("[PID:%d] Forking Reader %d -> %s\n", (int)getpid(), i, log_files[i]);
        fflush(stdout);

        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork reader");
            return 1;
        }
        if (pid == 0)
        {
            /* Child: Reader Process */
            /* Close all pipe read ends and other writers' write ends */
            for (int j = 0; j < num_files; j++)
            {
                close(pipe_fds[j][0]); /* read end */
                if (j != i)
                    close(pipe_fds[j][1]); /* other write ends */
            }
            reader_config_t rcfg;
            rcfg.reader_index = i;
            rcfg.num_threads = num_threads;
            rcfg.pipe_write_fd = pipe_fds[i][1];
            strncpy(rcfg.filepath, log_files[i], sizeof(rcfg.filepath) - 1);
            rcfg.filepath[sizeof(rcfg.filepath) - 1] = '\0';
            run_reader(&rcfg);
            /* run_reader calls _exit, never returns */
        }
        /* Parent */
        close(pipe_fds[i][1]); /* parent doesn't write */
        child_pids[num_children++] = pid;
    }

    /* ---- Fork Dispatcher ---- */
    printf("[PID:%d] Forking Dispatcher\n", (int)getpid());
    fflush(stdout);
    {
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork dispatcher");
            return 1;
        }
        if (pid == 0)
        {
            /* Close all pipes in dispatcher */
            for (int i = 0; i < num_files; i++)
            {
                close(pipe_fds[i][0]);
            }
            dispatcher_config_t dcfg;
            dcfg.filter_sources = filter_srcs;
            dcfg.num_filters = num_filters;
            dcfg.total_readers = num_files;
            dcfg.timeout_sec = timeout_sec;
            run_dispatcher(&dcfg);
        }
        child_pids[num_children++] = pid;
    }

    /* ---- Fork Analyzer Processes ---- */
    const char *lnames[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    for (int i = 0; i < 4; i++)
    {
        printf("[PID:%d] Forking Analyzer %s (index %d)\n", (int)getpid(), lnames[i], i);
        fflush(stdout);
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork analyzer");
            return 1;
        }
        if (pid == 0)
        {
            for (int j = 0; j < num_files; j++)
                close(pipe_fds[j][0]);
            analyzer_config_t acfg;
            acfg.level_index = i;
            acfg.num_workers = num_workers;
            acfg.keywords = keywords;
            acfg.num_keywords = num_keywords;
            acfg.timeout_sec = timeout_sec;
            run_analyzer(&acfg);
        }
        child_pids[num_children++] = pid;
    }

    /* ---- Fork Aggregator ---- */
    printf("[PID:%d] Forking Aggregator\n", (int)getpid());
    fflush(stdout);
    {
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork aggregator");
            return 1;
        }
        if (pid == 0)
        {
            for (int j = 0; j < num_files; j++)
                close(pipe_fds[j][0]);
            aggregator_config_t agcfg;
            agcfg.keywords = keywords;
            agcfg.num_keywords = num_keywords;
            agcfg.num_files = num_files;
            agcfg.timeout_sec = timeout_sec;
            agcfg.num_workers = num_workers;
            strncpy(agcfg.output_path, output_path, sizeof(agcfg.output_path) - 1);
            agcfg.output_path[sizeof(agcfg.output_path) - 1] = '\0';
            strncpy(agcfg.binary_path, binary_path, sizeof(agcfg.binary_path) - 1);
            agcfg.binary_path[sizeof(agcfg.binary_path) - 1] = '\0';
            strncpy(agcfg.filter_path, filter_file, sizeof(agcfg.filter_path) - 1);
            agcfg.filter_path[sizeof(agcfg.filter_path) - 1] = '\0';
            run_aggregator(&agcfg);
        }
        child_pids[num_children++] = pid;
    }

    /* ---- Start Watchdog Thread ---- */
    int *pipe_read_fds = (int *)malloc((size_t)num_files * sizeof(int));
    if (!pipe_read_fds)
    {
        perror("malloc");
        return 1;
    }
    for (int i = 0; i < num_files; i++)
        pipe_read_fds[i] = pipe_fds[i][0];

    watchdog_config_t wcfg;
    wcfg.pipe_read_fds = pipe_read_fds;
    wcfg.num_readers = num_files;
    wcfg.all_child_pids = child_pids;
    wcfg.num_children = num_children;
    wcfg.shutdown_flag = &shutdown_watchdog;

    pthread_t watchdog_tid;
    if (pthread_create(&watchdog_tid, NULL, watchdog_thread_func, &wcfg) != 0)
    {
        perror("pthread_create watchdog");
        return 1;
    }
    printf("[PID:%d] Watchdog thread started.\n", (int)getpid());
    fflush(stdout);

    /* ---- waitpid loop ---- */
    int children_remaining = num_children;
    while (children_remaining > 0)
    {
        if (sigint_received)
        {
            /* Send SIGTERM to all children */
            for (int i = 0; i < num_children; i++)
            {
                if (child_pids[i] > 0)
                    kill(child_pids[i], SIGTERM);
            }
            /* Wait with 5-second deadline using WNOHANG */
            time_t deadline = time(NULL) + 5;
            while (children_remaining > 0 && time(NULL) < deadline)
            {
                int status;
                pid_t p = waitpid(-1, &status, WNOHANG);
                if (p > 0)
                {
                    children_remaining--;
                    for (int i = 0; i < num_children; i++)
                    {
                        if (child_pids[i] == p)
                        {
                            child_pids[i] = -1;
                            break;
                        }
                    }
                }
                else if (p == 0)
                {
                    usleep(100000);
                }
                else
                {
                    if (errno == ECHILD)
                    {
                        children_remaining = 0;
                        break;
                    }
                }
            }
            break;
        }

        /* Use WNOHANG + short sleep so we can check sigint_received */
        int status;
        pid_t p = waitpid(-1, &status, WNOHANG);
        if (p > 0)
        {
            children_remaining--;
            for (int i = 0; i < num_children; i++)
            {
                if (child_pids[i] == p)
                {
                    child_pids[i] = -1;
                    break;
                }
            }
        }
        else if (p == 0)
        {
            /* No child exited yet — sleep briefly then re-check */
            usleep(50000);
        }
        else
        {
            if (errno == ECHILD)
                break;
            if (errno == EINTR)
                continue;
        }
    }

    if (sigint_received)
    {
        shutdown_watchdog = 1;
        pthread_join(watchdog_tid, NULL);

        for (int i = 0; i < num_children; i++)
        {
            if (child_pids[i] > 0)
                kill(child_pids[i], SIGKILL);
        }
        while (waitpid(-1, NULL, 0) > 0)
        {
        }

        for (int i = 0; i < num_files; i++)
        {
            if (pipe_read_fds[i] >= 0)
                close(pipe_read_fds[i]);
        }

        shm_force_cleanup();
        free(pipe_read_fds);
        free(pipe_fds);
        for (int i = 0; i < num_files; i++)
            free(log_files[i]);
        free(log_files);
        for (int i = 0; i < num_filters; i++)
            free(filter_srcs[i]);
        free(filter_srcs);
        for (int i = 0; i < num_keywords; i++)
            free(keywords[i]);
        free(keywords);
        _exit(1);
    }

    /* Collect final results from shared memory for summary */
    double total_weighted = 0.0;
    long total_entries = 0;
    for (int i = 0; i < 4; i++)
    {
        total_weighted += shm_c->results[i].total_weighted_score;
        total_entries += shm_c->results[i].total_entries;
    }
    double hp_score = shm_c->high_priority_score;

    /* ---- Stop Watchdog ---- */
    shutdown_watchdog = 1;
    pthread_join(watchdog_tid, NULL);

    /* Close remaining pipe read ends */
    for (int i = 0; i < num_files; i++)
    {
        if (pipe_read_fds[i] >= 0)
            close(pipe_read_fds[i]);
    }

    /* ---- Final summary ---- */
    printf("==================================================\n");
    printf("SYSTEM SUMMARY\n");
    printf("Keywords  : %s\n", keywords_str);
    printf("Log files : %d\n", num_files);
    printf("Total entries  : %ld\n", total_entries);
    printf("Total weighted : %.1f\n", total_weighted);
    printf("High-priority  : %.1f (source filter: %s)\n", hp_score, filter_file);
    const char *lns[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    for (int i = 0; i < 4; i++)
    {
        printf("  %-6s: %ld entries, score: %.1f\n",
               lns[i],
               shm_c->results[i].total_entries,
               shm_c->results[i].total_weighted_score);
    }
    printf("==================================================\n");
    printf("Program terminated successfully.\n");
    fflush(stdout);

    /* ---- Cleanup ---- */
    shm_destroy();
    free(pipe_read_fds);
    free(pipe_fds);
    for (int i = 0; i < num_files; i++)
        free(log_files[i]);
    free(log_files);
    for (int i = 0; i < num_filters; i++)
        free(filter_srcs[i]);
    free(filter_srcs);
    for (int i = 0; i < num_keywords; i++)
        free(keywords[i]);
    free(keywords);
    _exit(0);
}
