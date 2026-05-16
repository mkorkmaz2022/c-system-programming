#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <stdbool.h>
#include <signal.h>
#include <errno.h>

#define FLAG_F 1
#define FLAG_B 2
#define FLAG_T 4
#define FLAG_P 8
#define FLAG_L 16

typedef struct
{
    char *dir_path;
    char *filename;
    int size;
    char type;
    char *permissions;
    int links;
    int flags;
} SearchCriteria;

typedef struct TreeNode
{
    char *name;
    char *path;
    bool is_match;
    bool has_match;
    struct TreeNode **children;
    int num_children;
    int capacity;
} TreeNode;

// Global var. (Sinyal catch signal, clean etc.)
TreeNode *global_root = NULL;
SearchCriteria criteria = {0};

void print_usage_and_exit()
{
    fprintf(stderr, "Usage: ./myFind -w targetDirectoryPath [-f filename_regex] [-b size] [-t type] [-p permissions] [-l links]\n");
    fprintf(stderr, "At least one search criteria (-f, -b, -t, -p, -l) must be provided.\n");
    exit(1);
}

void error_exit(const char *msg)
{
    fprintf(stderr, "[ERROR] %s: %s\n", msg, strerror(errno));
    exit(1);
}

// Regex fonksiyonu: Only supports '+' operator,it is case-insensitive
bool match_regex(const char *pattern, const char *str)
{
    int p = 0, s = 0;
    int p_len = strlen(pattern);
    int s_len = strlen(str);

    while (p < p_len && s < s_len)
    {
        char p_char = tolower(pattern[p]);
        bool is_plus = (p + 1 < p_len && pattern[p + 1] == '+');

        if (is_plus)
        {
            int count = 0;
            while (s < s_len && tolower(str[s]) == p_char)
            {
                count++;
                s++;
            }
            if (count == 0)
                return false; // '+' requires at least one character.
            p += 2;
        }
        else
        {
            if (tolower(str[s]) != p_char)
                return false;
            p++;
            s++;
        }
    }

    while (p < p_len && p + 1 < p_len && pattern[p + 1] == '+')
    {
        return false;
    }
    return (p == p_len && s == s_len);
}

// COnverts the permissions to string
char *get_perm_string(mode_t mode)
{
    static char perms[10];
    perms[0] = (mode & S_IRUSR) ? 'r' : '-';
    perms[1] = (mode & S_IWUSR) ? 'w' : '-';
    perms[2] = (mode & S_IXUSR) ? 'x' : '-';
    perms[3] = (mode & S_IRGRP) ? 'r' : '-';
    perms[4] = (mode & S_IWGRP) ? 'w' : '-';
    perms[5] = (mode & S_IXGRP) ? 'x' : '-';
    perms[6] = (mode & S_IROTH) ? 'r' : '-';
    perms[7] = (mode & S_IWOTH) ? 'w' : '-';
    perms[8] = (mode & S_IXOTH) ? 'x' : '-';
    perms[9] = '\0';
    return perms;
}

// Creating node
TreeNode *create_node(const char *name, const char *path)
{
    TreeNode *node = malloc(sizeof(TreeNode));
    if (!node)
        error_exit("Memory allocation failed");
    node->name = strdup(name);
    node->path = strdup(path);
    node->is_match = false;
    node->has_match = false;
    node->num_children = 0;
    node->capacity = 10;
    node->children = malloc(node->capacity * sizeof(TreeNode *));
    if (!node->children)
        error_exit("Memory allocation failed");
    return node;
}

void add_child(TreeNode *parent, TreeNode *child)
{
    if (parent->num_children >= parent->capacity)
    {
        parent->capacity *= 2;
        parent->children = realloc(parent->children, parent->capacity * sizeof(TreeNode *));
        if (!parent->children)
            error_exit("Memory reallocation failed");
    }
    parent->children[parent->num_children++] = child;
}

// To avoid memory leaks cleans the tree
void free_tree(TreeNode *node)
{
    if (!node)
        return;
    for (int i = 0; i < node->num_children; i++)
    {
        free_tree(node->children[i]);
    }
    free(node->children);
    free(node->name);
    free(node->path);
    free(node);
}

// catches signal
void handle_sigint(int sig)
{
    (void)sig; // unused parameter
    fprintf(stderr, "\n[INFO] Execution interrupted by user (CTRL-C). Cleaning up resources...\n");
    free_tree(global_root);
    exit(0);
}

// Checks the target file based on its properties.
bool check_criteria(const char *filename, struct stat *file_stat)
{
    if (criteria.flags & FLAG_F)
    {
        if (!match_regex(criteria.filename, filename))
            return false;
    }
    if (criteria.flags & FLAG_B)
    {
        if (file_stat->st_size != criteria.size)
            return false;
    }
    if (criteria.flags & FLAG_T)
    {
        char type = criteria.type;
        mode_t m = file_stat->st_mode;
        if (type == 'd' && !S_ISDIR(m))
            return false;
        if (type == 's' && !S_ISSOCK(m))
            return false;
        if (type == 'b' && !S_ISBLK(m))
            return false;
        if (type == 'c' && !S_ISCHR(m))
            return false;
        if (type == 'f' && !S_ISREG(m))
            return false;
        if (type == 'p' && !S_ISFIFO(m))
            return false;
        if (type == 'l' && !S_ISLNK(m))
            return false;
    }
    if (criteria.flags & FLAG_P)
    {
        if (strcmp(criteria.permissions, get_perm_string(file_stat->st_mode)) != 0)
            return false;
    }
    if (criteria.flags & FLAG_L)
    {
        if (file_stat->st_nlink != (nlink_t)criteria.links)
            return false;
    }
    return true;
}

// traverse directory recursively
void traverse_directory(TreeNode *current_node)
{
    DIR *dir = opendir(current_node->path);
    if (!dir)
    {
        fprintf(stderr, "[WARNING] Cannot open directory %s: %s\n", current_node->path, strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char path_buffer[1024];
        snprintf(path_buffer, sizeof(path_buffer), "%s/%s", current_node->path, entry->d_name);

        struct stat file_stat;
        if (lstat(path_buffer, &file_stat) == -1)
        {
            fprintf(stderr, "[WARNING] Cannot stat %s: %s\n", path_buffer, strerror(errno));
            continue;
        }

        TreeNode *child_node = create_node(entry->d_name, path_buffer);
        add_child(current_node, child_node);

        if (check_criteria(entry->d_name, &file_stat))
        {
            child_node->is_match = true;
            child_node->has_match = true;
            current_node->has_match = true;
        }

        if (S_ISDIR(file_stat.st_mode))
        {
            traverse_directory(child_node);
            if (child_node->has_match)
            {
                current_node->has_match = true;
            }
        }
    }
    closedir(dir);
}

// Printing the output as a properly formatted tree.
void print_tree(TreeNode *node, int depth)
{
    // If there is no match in this directory and it is not the root directory, do not print anything to the screen.
    if (!node->has_match && !node->is_match && depth != 0)
        return;

    if (depth == 0)
    {
        // Root directory: Just type the name without putting anything at the beginning.
        printf("%s\n", node->name);
    }
    else
    {
        // Subdirectories/files: Start with '|', then add '--' for the depth level.
        printf("|");
        for (int i = 0; i < depth; i++)
        {
            printf("--");
        }
        printf("%s\n", node->name);
    }

    // Trace the child knots and continue recursively, increasing the depth by 1.
    for (int i = 0; i < node->num_children; i++)
    {
        print_tree(node->children[i], depth + 1);
    }
}

int main(int argc, char *argv[])
{
    int opt;

    // Signal configuration
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1)
    {
        error_exit("Cannot setup SIGINT handler");
    }

    while ((opt = getopt(argc, argv, "w:f:b:t:p:l:")) != -1)
    {
        switch (opt)
        {
        case 'w':
            criteria.dir_path = optarg;
            break;
        case 'f':
            criteria.filename = optarg;
            criteria.flags |= FLAG_F;
            break;
        case 'b':
            criteria.size = atoi(optarg);
            criteria.flags |= FLAG_B;
            break;
        case 't':
            criteria.type = optarg[0];
            criteria.flags |= FLAG_T;
            break;
        case 'p':
            if (strlen(optarg) != 9)
            {
                fprintf(stderr, "[ERROR] Permissions must be exactly 9 characters (e.g., rwxr-xr--)\n");
                exit(1);
            }
            criteria.permissions = optarg;
            criteria.flags |= FLAG_P;
            break;
        case 'l':
            criteria.links = atoi(optarg);
            criteria.flags |= FLAG_L;
            break;
        default:
            print_usage_and_exit();
        }
    }

    if (!criteria.dir_path || criteria.flags == 0)
    {
        print_usage_and_exit();
    }

    // Checking the starting directory
    struct stat root_stat;
    if (lstat(criteria.dir_path, &root_stat) == -1)
    {
        error_exit("Target directory cannot be accessed");
    }
    if (!S_ISDIR(root_stat.st_mode))
    {
        fprintf(stderr, "[ERROR] Path is not a directory.\n");
        exit(1);
    }

    global_root = create_node(criteria.dir_path, criteria.dir_path);

    // Does the root directory itself meet the criteria?
    if (check_criteria(criteria.dir_path, &root_stat))
    {
        global_root->is_match = true;
        global_root->has_match = true;
    }

    traverse_directory(global_root);

    if (!global_root->has_match)
    {
        printf("No file found\n");
    }
    else
    {
        print_tree(global_root, 0);
    }

    free_tree(global_root);
    return 0;
}