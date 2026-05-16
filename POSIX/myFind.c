#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

char *pattern = NULL;
char *perm = NULL;
int size_filter = -1;
int link_filter = -1;
char type_filter = 0;

int match_pattern(const char *p, const char *s)
{
    if (!*p && !*s)
        return 1;

    if (*(p + 1) == '+')
    {
        if (tolower(*p) != tolower(*s))
            return 0;

        while (tolower(*s) == tolower(*p))
            s++;

        return match_pattern(p + 2, s);
    }

    if (tolower(*p) == tolower(*s))
        return match_pattern(p + 1, s + 1);

    return 0;
}

int check_type(struct stat *st)
{
    if (!type_filter)
        return 1;

    switch (type_filter)
    {
    case 'f':
        return S_ISREG(st->st_mode);
    case 'd':
        return S_ISDIR(st->st_mode);
    case 'l':
        return S_ISLNK(st->st_mode);
    case 'b':
        return S_ISBLK(st->st_mode);
    case 'c':
        return S_ISCHR(st->st_mode);
    case 'p':
        return S_ISFIFO(st->st_mode);
    case 's':
        return S_ISSOCK(st->st_mode);
    }

    return 0;
}

int check_perm(struct stat *st)
{
    if (!perm)
        return 1;

    char p[10] = "---------";

    if (st->st_mode & S_IRUSR)
        p[0] = 'r';
    if (st->st_mode & S_IWUSR)
        p[1] = 'w';
    if (st->st_mode & S_IXUSR)
        p[2] = 'x';

    if (st->st_mode & S_IRGRP)
        p[3] = 'r';
    if (st->st_mode & S_IWGRP)
        p[4] = 'w';
    if (st->st_mode & S_IXGRP)
        p[5] = 'x';

    if (st->st_mode & S_IROTH)
        p[6] = 'r';
    if (st->st_mode & S_IWOTH)
        p[7] = 'w';
    if (st->st_mode & S_IXOTH)
        p[8] = 'x';

    return strcmp(p, perm) == 0;
}

void print_prefix(int depth)
{
    for (int i = 0; i < depth; i++)
        printf("│   ");
}

void search(const char *path, int depth)
{
    DIR *dir = opendir(path);
    if (!dir)
        return;

    struct dirent *entry;

    while ((entry = readdir(dir)))
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;

        char full[4096];
        sprintf(full, "%s/%s", path, entry->d_name);

        struct stat st;
        lstat(full, &st);

        int ok = 1;

        if (pattern && !match_pattern(pattern, entry->d_name))
            ok = 0;

        if (size_filter != -1 && st.st_size != size_filter)
            ok = 0;

        if (link_filter != -1 && st.st_nlink != link_filter)
            ok = 0;

        if (!check_type(&st))
            ok = 0;

        if (!check_perm(&st))
            ok = 0;

        if (ok)
        {
            print_prefix(depth);
            printf("├── %s\n", entry->d_name);
        }

        if (S_ISDIR(st.st_mode))
            search(full, depth + 1);
    }

    closedir(dir);
}

int main(int argc, char *argv[])
{
    char *path = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "w:f:b:t:p:l:")) != -1)
    {
        switch (opt)
        {
        case 'w':
            path = optarg;
            break;
        case 'f':
            pattern = optarg;
            break;
        case 'b':
            size_filter = atoi(optarg);
            break;
        case 't':
            type_filter = optarg[0];
            break;
        case 'p':
            perm = optarg;
            break;
        case 'l':
            link_filter = atoi(optarg);
            break;
        }
    }

    if (!path)
    {
        printf("Usage: ./myFind -w path\n");
        return 1;
    }

    printf("%s\n", path);

    search(path, 0);

    return 0;
}