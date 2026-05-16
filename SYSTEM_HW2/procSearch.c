#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>

#define MAX_WORKERS 8
#define MAX_PATH 1024
#define MAX_MATCHES 10000

// GLOBAL VARIABLES FOR SIGNAL HANDLERS (Async-Signal-Safe)
volatile sig_atomic_t sigint_received = 0;
volatile sig_atomic_t sigusr1_count = 0;
volatile sig_atomic_t worker_sigterm_received = 0;

pid_t worker_pids[MAX_WORKERS];
int num_active_workers = 0;
volatile sig_atomic_t normal_exit_pids[MAX_WORKERS];

// DATA STRUCTURES
typedef struct
{
    char path[MAX_PATH];
    off_t size;
    pid_t worker_pid;
} MatchNode;

MatchNode all_matches[MAX_MATCHES];
int total_matches = 0;
int total_scanned = 0;

// FUNCTION PROTOTYPES
void handle_sigusr1(int sig, siginfo_t *info, void *context);
void handle_sigint(int sig);
void handle_sigchld(int sig);
void handle_worker_sigterm(int sig);
bool match_regex_exact(const char *pat, const char *str);
bool match_regex(const char *pat, const char *str);
void worker_process(char **subdirs, int num_dirs, const char *pattern, off_t min_size, int worker_index);
void parent_search_root(const char *root_dir, const char *pattern, off_t min_size);
int compare_matches(const void *a, const void *b);
void print_tree(const char *root_dir);
int get_depth(const char *root, const char *path);

// SIGNAL HANDLERS
void handle_sigusr1(int sig, siginfo_t *info, void *context)
{
    (void)sig;
    (void)context;
    if (info && info->si_pid > 0)
    {
        for (int i = 0; i < MAX_WORKERS; i++)
        {
            if (worker_pids[i] == info->si_pid)
            {
                normal_exit_pids[i] = 1;
                break;
            }
        }
    }
    sigusr1_count++;
}

void handle_sigint(int sig)
{
    (void)sig;
    sigint_received = 1;
}

void handle_sigchld(int sig)
{
    (void)sig;
    int saved_errno = errno;
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        int is_normal = 0;
        for (int i = 0; i < MAX_WORKERS; i++)
        {
            if (worker_pids[i] == pid)
            {
                if (normal_exit_pids[i] == 1)
                {
                    is_normal = 1;
                }
                break;
            }
        }

        if (!is_normal && !sigint_received)
        {
            char msg[128];
            int len = snprintf(msg, sizeof(msg), "[Parent] Worker PID: %d terminated unexpectedly (exit status: %d).\n", pid, WEXITSTATUS(status));
            ssize_t dummy = write(STDERR_FILENO, msg, len);
            (void)dummy;
            sigusr1_count++;
        }
    }
    errno = saved_errno;
}

void handle_worker_sigterm(int sig)
{
    (void)sig;
    worker_sigterm_received = 1;
}

// REGEX IMPLEMENTATION
bool match_regex_exact(const char *pat, const char *str)
{
    if (*pat == '\0')
        return true;

    bool is_plus = (*(pat + 1) == '+');

    if (is_plus)
    {
        if (*str != '\0' && tolower(*str) == tolower(*pat))
        {
            return match_regex_exact(pat + 2, str + 1) || match_regex_exact(pat, str + 1);
        }
        return false;
    }
    else
    {
        if (*str != '\0' && tolower(*str) == tolower(*pat))
        {
            return match_regex_exact(pat + 1, str + 1);
        }
        return false;
    }
}

bool match_regex(const char *pat, const char *str)
{
    while (*str != '\0')
    {
        if (match_regex_exact(pat, str))
            return true;
        str++;
    }
    return false;
}

// WORKER LOGIC
void worker_process(char **subdirs, int num_dirs, const char *pattern, off_t min_size, int worker_index)
{
    struct sigaction sa_ign;
    memset(&sa_ign, 0, sizeof(sa_ign));
    sa_ign.sa_handler = SIG_IGN;
    sigemptyset(&sa_ign.sa_mask);
    sigaction(SIGINT, &sa_ign, NULL);

    int match_count = 0;
    int scanned_count = 0;
    pid_t my_pid = getpid();

    char temp_filename[64];
    snprintf(temp_filename, sizeof(temp_filename), "worker_%d.txt", my_pid);
    FILE *f = fopen(temp_filename, "w");
    if (!f)
        exit(1);

    fprintf(f, "Scanned: 0000000000\n");

    for (int i = 0; i < num_dirs; i++)
    {
        if (worker_sigterm_received)
            break;

        char path_stack[2048][MAX_PATH];
        int top = 0;

        size_t slen = strlen(subdirs[i]);
        if (slen >= MAX_PATH)
            slen = MAX_PATH - 1;
        memcpy(path_stack[top], subdirs[i], slen);
        path_stack[top][slen] = '\0';
        top++;

        while (top > 0)
        {
            if (worker_sigterm_received)
                break;
            char current_path[MAX_PATH];

            top--;
            size_t clen = strlen(path_stack[top]);
            memcpy(current_path, path_stack[top], clen + 1);

            DIR *dir = opendir(current_path);
            if (!dir)
                continue;

            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL)
            {
                if (worker_sigterm_received)
                    break;
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                    continue;
                scanned_count++;

                char full_path[MAX_PATH];
                int req_len = snprintf(full_path, sizeof(full_path), "%s/%s", current_path, entry->d_name);
                if (req_len >= MAX_PATH || req_len < 0)
                    continue;

                struct stat st;
                if (lstat(full_path, &st) == -1)
                    continue;

                if (S_ISDIR(st.st_mode))
                {
                    if (top < 2048)
                    {
                        size_t flen = strlen(full_path);
                        memcpy(path_stack[top], full_path, flen + 1);
                        top++;
                    }
                }
                else if (S_ISREG(st.st_mode))
                {
                    if (match_regex(pattern, entry->d_name) && st.st_size >= min_size)
                    {
                        match_count++;
                        printf("[Worker PID:%d] MATCH: %s (%ld bytes)\n", my_pid, full_path, (long)st.st_size);
                        fprintf(f, "%s|%ld\n", full_path, (long)st.st_size);
                    }
                }
            }
            closedir(dir);
        }
    }

    fseek(f, 0, SEEK_SET);
    fprintf(f, "Scanned: %010d\n", scanned_count);
    fclose(f);

    if (worker_sigterm_received)
    {
        printf("[Worker PID: %d] SIGTERM received. Partial matches: %d. Exiting.\n", my_pid, match_count);
        exit(match_count % 256);
    }
    else
    {
        usleep(worker_index * 25000);
        kill(getppid(), SIGUSR1);
        usleep(50000);
        exit(match_count % 256);
    }
}

// PARENT DIRECTORY SEARCH
void parent_search_root(const char *root_dir, const char *pattern, off_t min_size)
{
    char temp_filename[64];
    snprintf(temp_filename, sizeof(temp_filename), "worker_%d.txt", getpid());
    FILE *f = fopen(temp_filename, "w");
    if (!f)
        return;

    int scanned_count = 0;
    fprintf(f, "Scanned: 0000000000\n");

    DIR *dir = opendir(root_dir);
    if (dir)
    {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            scanned_count++;

            char full_path[MAX_PATH];
            int req_len = snprintf(full_path, sizeof(full_path), "%s/%s", root_dir, entry->d_name);
            if (req_len >= MAX_PATH || req_len < 0)
                continue;

            struct stat st;
            if (lstat(full_path, &st) == 0 && S_ISREG(st.st_mode))
            {
                if (match_regex(pattern, entry->d_name) && st.st_size >= min_size)
                {
                    fprintf(f, "%s|%ld\n", full_path, (long)st.st_size);
                }
            }
        }
        closedir(dir);
    }

    fseek(f, 0, SEEK_SET);
    fprintf(f, "Scanned: %010d\n", scanned_count);
    fclose(f);
}

// TREE AND SORT UTILS
int compare_matches(const void *a, const void *b)
{
    return strcmp(((MatchNode *)a)->path, ((MatchNode *)b)->path);
}

void get_dir_path(const char *full_path, char *dir_path)
{
    size_t len = strlen(full_path);
    if (len >= MAX_PATH)
        len = MAX_PATH - 1;
    memcpy(dir_path, full_path, len);
    dir_path[len] = '\0';
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash)
        *last_slash = '\0';
}

int get_depth(const char *root, const char *path)
{
    const char *rel = path + strlen(root);
    while (*rel == '/')
        rel++;
    int depth = 1;
    for (const char *p = rel; *p; p++)
    {
        if (*p == '/' && *(p + 1) != '\0' && *(p + 1) != '/')
            depth++;
    }
    return depth;
}

void print_tree(const char *root_dir)
{
    qsort(all_matches, total_matches, sizeof(MatchNode), compare_matches);

    printf("\n%s\n", root_dir);
    if (total_matches == 0)
    {
        printf("No matching files found.\n");
        return;
    }

    char last_printed_dir[MAX_PATH] = "";

    for (int i = 0; i < total_matches; i++)
    {
        char current_dir[MAX_PATH];
        get_dir_path(all_matches[i].path, current_dir);

        if (strcmp(current_dir, last_printed_dir) != 0)
        {
            char *token;
            char temp_path[MAX_PATH] = "";
            char dir_copy[MAX_PATH];

            size_t cd_len = strlen(current_dir);
            memcpy(dir_copy, current_dir, cd_len + 1);

            int depth = 0;
            char *rel_path = dir_copy;
            if (strncmp(dir_copy, root_dir, strlen(root_dir)) == 0)
            {
                rel_path += strlen(root_dir);
                if (rel_path[0] == '/')
                    rel_path++;
            }

            token = strtok(rel_path, "/");
            while (token != NULL)
            {
                depth++;
                int dashes = (depth == 1) ? 2 : 2 + (depth - 1) * 6;

                if (strlen(temp_path) > 0)
                {
                    strncat(temp_path, "/", MAX_PATH - strlen(temp_path) - 1);
                }
                strncat(temp_path, token, MAX_PATH - strlen(temp_path) - 1);

                if (strstr(last_printed_dir, temp_path) == NULL)
                {
                    printf("|");
                    for (int d = 0; d < dashes; d++)
                        printf("-");
                    printf(" %s\n", token);
                }
                token = strtok(NULL, "/");
            }
            size_t lpd_len = strlen(current_dir);
            memcpy(last_printed_dir, current_dir, lpd_len + 1);
        }

        char *filename = strrchr(all_matches[i].path, '/');
        filename = filename ? filename + 1 : all_matches[i].path;

        int file_depth = get_depth(root_dir, all_matches[i].path);
        int dashes = (file_depth == 1) ? 2 : 2 + (file_depth - 1) * 6;

        printf("|");
        for (int d = 0; d < dashes; d++)
            printf("-");
        printf(" %s (%ld bytes) [Worker %d]\n", filename, (long)all_matches[i].size, all_matches[i].worker_pid);
    }
}

// MAIN
int main(int argc, char *argv[])
{
    int opt;
    char *root_dir = NULL;
    char *pattern = NULL;
    int req_workers = 0;
    off_t min_size = 0;

    while ((opt = getopt(argc, argv, "d:n:f:s:")) != -1)
    {
        switch (opt)
        {
        case 'd':
            root_dir = optarg;
            break;
        case 'n':
            req_workers = atoi(optarg);
            break;
        case 'f':
            pattern = optarg;
            break;
        case 's':
            min_size = atol(optarg);
            break;
        default:
            fprintf(stderr, "Usage: ./procSearch -d <root dir> -n <num workers> -f <pattern> [-s <min size bytes>]\n");
            exit(1);
        }
    }

    if (!root_dir || !pattern || req_workers < 2 || req_workers > 8)
    {
        fprintf(stderr, "Usage: ./procSearch -d <root dir> -n <num workers (2-8)> -f <pattern> [-s <min size bytes>]\n");
        exit(1);
    }

    struct stat root_st;
    if (stat(root_dir, &root_st) != 0 || !S_ISDIR(root_st.st_mode))
    {
        fprintf(stderr, "Error: Root directory does not exist or is not a directory.\n");
        exit(1);
    }

    struct sigaction sa_usr1;
    memset(&sa_usr1, 0, sizeof(sa_usr1));
    sa_usr1.sa_sigaction = handle_sigusr1;
    sigemptyset(&sa_usr1.sa_mask);
    sa_usr1.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGUSR1, &sa_usr1, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = handle_sigchld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa_chld, NULL);

    int subdirs_capacity = 1024;
    char **subdirs = malloc(subdirs_capacity * sizeof(char *));
    int subdir_count = 0;

    DIR *dir = opendir(root_dir);
    if (dir)
    {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            char full_path[MAX_PATH];
            int req_len = snprintf(full_path, sizeof(full_path), "%s/%s", root_dir, entry->d_name);
            if (req_len >= MAX_PATH || req_len < 0)
                continue;

            struct stat st;
            if (lstat(full_path, &st) == 0 && S_ISDIR(st.st_mode))
            {
                if (subdir_count >= subdirs_capacity)
                {
                    subdirs_capacity *= 2;
                    subdirs = realloc(subdirs, subdirs_capacity * sizeof(char *));
                }
                subdirs[subdir_count++] = strdup(full_path);
            }
        }
        closedir(dir);
    }

    if (subdir_count == 0)
    {
        printf("Notice: no subdirectories found; parent will search root directly.\n");
        num_active_workers = 0;
    }
    else if (subdir_count < req_workers)
    {
        printf("Notice: only %d subdirectories found; using %d workers instead of %d.\n", subdir_count, subdir_count, req_workers);
        num_active_workers = subdir_count;
    }
    else
    {
        num_active_workers = req_workers;
    }

    sigset_t block_mask, old_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGUSR1);
    sigaddset(&block_mask, SIGCHLD);

    sigprocmask(SIG_BLOCK, &block_mask, &old_mask);

    for (int i = 0; i < num_active_workers; i++)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            sigprocmask(SIG_SETMASK, &old_mask, NULL);

            int assigned_count = 0;
            char **my_subdirs = malloc(subdir_count * sizeof(char *));
            for (int j = i; j < subdir_count; j += num_active_workers)
            {
                my_subdirs[assigned_count++] = subdirs[j];
            }
            worker_process(my_subdirs, assigned_count, pattern, min_size, i);
            free(my_subdirs);
            exit(0);
        }
        else if (pid > 0)
        {
            worker_pids[i] = pid;
        }
        else
        {
            perror("fork failed");
            exit(1);
        }
    }

    sigprocmask(SIG_SETMASK, &old_mask, NULL);
    parent_search_root(root_dir, pattern, min_size);

    sigprocmask(SIG_BLOCK, &block_mask, NULL);
    while (sigusr1_count < num_active_workers && !sigint_received)
    {
        sigsuspend(&old_mask);
    }
    sigprocmask(SIG_SETMASK, &old_mask, NULL);

    bool is_partial = false;

    if (sigint_received)
    {
        printf("\n[Parent] SIGINT received. Terminating all workers...\n");
        // fprintf(stderr, "\n[Parent] SIGINT received. Terminating all workers...\n");
        // fflush(stderr);
        for (int i = 0; i < num_active_workers; i++)
        {
            kill(worker_pids[i], SIGTERM);
        }
        sleep(3);
        for (int i = 0; i < num_active_workers; i++)
        {
            int status;
            if (waitpid(worker_pids[i], &status, WNOHANG) == 0)
            {
                kill(worker_pids[i], SIGKILL);
                waitpid(worker_pids[i], &status, 0);
            }
        }
        is_partial = true;
    }
    else
    {
        for (int i = 0; i < num_active_workers; i++)
        {
            int status;
            waitpid(worker_pids[i], &status, 0);
        }
    }

    int worker_matches[MAX_WORKERS] = {0};

    char parent_temp[64];
    snprintf(parent_temp, sizeof(parent_temp), "worker_%d.txt", getpid());
    FILE *pf = fopen(parent_temp, "r");
    if (pf)
    {
        char line[MAX_PATH + 64];
        if (fgets(line, sizeof(line), pf))
        {
            sscanf(line, "Scanned: %d", &total_scanned);
        }
        while (fgets(line, sizeof(line), pf))
        {
            char *sep = strchr(line, '|');
            if (sep && total_matches < MAX_MATCHES)
            {
                *sep = '\0';
                size_t plen = strlen(line);
                if (plen >= MAX_PATH)
                    plen = MAX_PATH - 1;
                memcpy(all_matches[total_matches].path, line, plen);
                all_matches[total_matches].path[plen] = '\0';
                all_matches[total_matches].size = atol(sep + 1);
                all_matches[total_matches].worker_pid = getpid();
                total_matches++;
            }
        }
        fclose(pf);
        remove(parent_temp);
    }

    for (int i = 0; i < num_active_workers; i++)
    {
        char temp_filename[64];
        snprintf(temp_filename, sizeof(temp_filename), "worker_%d.txt", worker_pids[i]);
        FILE *f = fopen(temp_filename, "r");
        if (f)
        {
            char line[MAX_PATH + 64];
            if (fgets(line, sizeof(line), f))
            {
                int ws = 0;
                sscanf(line, "Scanned: %d", &ws);
                total_scanned += ws;
            }
            while (fgets(line, sizeof(line), f))
            {
                char *sep = strchr(line, '|');
                if (sep && total_matches < MAX_MATCHES)
                {
                    *sep = '\0';
                    size_t plen = strlen(line);
                    if (plen >= MAX_PATH)
                        plen = MAX_PATH - 1;
                    memcpy(all_matches[total_matches].path, line, plen);
                    all_matches[total_matches].path[plen] = '\0';
                    all_matches[total_matches].size = atol(sep + 1);
                    all_matches[total_matches].worker_pid = worker_pids[i];
                    total_matches++;
                    worker_matches[i]++;
                }
            }
            fclose(f);
            remove(temp_filename);
        }
    }

    print_tree(root_dir);

    printf("\n--- %sSummary ---\n", is_partial ? "Partial " : "");
    printf("Total workers used : %d\n", num_active_workers);
    printf("Total files scanned: %d\n", total_scanned);
    printf("Total matches found: %d\n", total_matches);
    for (int i = 0; i < num_active_workers; i++)
    {
        printf("Worker PID %d : %d match%s\n", worker_pids[i], worker_matches[i], (worker_matches[i] == 1) ? "" : "es");
    }

    for (int i = 0; i < subdir_count; i++)
        free(subdirs[i]);
    free(subdirs);

    return is_partial ? 1 : 0;
}