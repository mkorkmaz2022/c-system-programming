#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_WORDS 1000
#define MAX_WORD_LEN 64
#define MAX_FLOORS 50
#define MAX_TASKS 5000
#define IDLE_SLEEP_USEC 120000
#define ACTIVE_SLEEP_USEC 30000

typedef struct
{
    int word_id;
    char original_word[MAX_WORD_LEN];
    int length;
    int sorting_floor;
    int arrival_floor;

    char sorting_area[MAX_WORD_LEN];
    int occupied[MAX_WORD_LEN];
    int fixed[MAX_WORD_LEN];

    int claimed;
    int admitted;
    int completed;

    sem_t word_mutex;
} Word;

typedef struct
{
    int word_index;
    char character;
    int original_index;
    int source_floor;
    int dest_floor;
    int is_claimed;
    int is_delivered;
} CharTask;

typedef struct
{
    int num_floors;
    int max_words_per_floor;

    Word words[MAX_WORDS];
    int total_words_in_file;
    int next_word_to_read;
    sem_t admission_mutex;

    int active_words_on_floor[MAX_FLOORS];

    CharTask tasks[MAX_TASKS];
    int total_tasks;
    sem_t task_mutex;

    int delivery_elevator_floor;
    int delivery_elevator_dir;
    int delivery_elevator_count;
    sem_t delivery_mutex;

    int reposition_elevator_floor;
    int reposition_elevator_dir;
    int reposition_elevator_count;
    sem_t reposition_mutex;

    int total_completed_words;
    int total_retries;
    int total_chars_transported;
    sem_t stats_mutex;

    sem_t floor_wait_delivery[MAX_FLOORS];
    sem_t ride_wait_delivery[MAX_FLOORS];
    int waiting_at_floor[MAX_FLOORS];
    int dropping_at_floor[MAX_FLOORS];

    sem_t floor_wait_reposition[MAX_FLOORS];
    int waiting_reposition_at_floor[MAX_FLOORS];

    int terminate_flag;
} SharedMemory;

static SharedMemory *shm = NULL;
static volatile sig_atomic_t sigint_received = 0;
static char *output_file_global = NULL;
static int input_line_count = 0;

static void handle_sigint(int sig)
{
    (void)sig;
    sigint_received = 1;
    if (shm)
    {
        shm->terminate_flag = 1;
    }
}

static void cleanup_and_exit(int status)
{
    if (shm)
    {
        sem_destroy(&shm->admission_mutex);
        sem_destroy(&shm->task_mutex);
        sem_destroy(&shm->delivery_mutex);
        sem_destroy(&shm->reposition_mutex);
        sem_destroy(&shm->stats_mutex);
        for (int i = 0; i < shm->total_words_in_file; i++)
        {
            sem_destroy(&shm->words[i].word_mutex);
        }
        for (int i = 0; i < shm->num_floors; i++)
        {
            sem_destroy(&shm->floor_wait_delivery[i]);
            sem_destroy(&shm->ride_wait_delivery[i]);
            sem_destroy(&shm->floor_wait_reposition[i]);
        }
        munmap(shm, sizeof(SharedMemory));
    }
    exit(status);
}

static void truncate_output_file(void)
{
    if (!output_file_global)
    {
        return;
    }
    FILE *out = fopen(output_file_global, "w");
    if (out)
    {
        fclose(out);
    }
}

static bool is_valid_lowercase_word(const char *word)
{
    if (!word || word[0] == '\0')
    {
        return false;
    }
    for (int i = 0; word[i] != '\0'; i++)
    {
        unsigned char ch = (unsigned char)word[i];
        if (!islower(ch))
        {
            return false;
        }
    }
    return true;
}

static bool any_incomplete_words(void)
{
    for (int i = 0; i < shm->total_words_in_file; i++)
    {
        if (shm->words[i].completed == 0)
        {
            return true;
        }
    }
    return false;
}

static bool any_pending_tasks(void)
{
    bool pending = false;
    sem_wait(&shm->task_mutex);
    for (int i = 0; i < shm->total_tasks; i++)
    {
        if (shm->tasks[i].is_delivered == 0)
        {
            pending = true;
            break;
        }
    }
    sem_post(&shm->task_mutex);
    return pending;
}

static void word_carrier_process(int initial_floor)
{
    printf("[PID:%d] Word-carrier-process initialized on floor %d\n", getpid(), initial_floor);

    while (!shm->terminate_flag)
    {
        int selected_word_index = -1;
        bool capacity_failed = false;

        sem_wait(&shm->admission_mutex);
        for (int i = 0; i < shm->total_words_in_file; i++)
        {
            int index = (shm->next_word_to_read + i) % shm->total_words_in_file;
            if (shm->words[index].claimed == 0 && shm->words[index].admitted == 0 && shm->words[index].completed == 0)
            {
                shm->words[index].claimed = 1;
                selected_word_index = index;
                shm->next_word_to_read = (index + 1) % shm->total_words_in_file;
                break;
            }
        }

        if (selected_word_index != -1)
        {
            Word *w = &shm->words[selected_word_index];
            int s_floor = w->sorting_floor;
            int a_floor = initial_floor;
            bool capacity_ok = false;

            if (a_floor == s_floor)
            {
                if (shm->active_words_on_floor[a_floor] < shm->max_words_per_floor)
                {
                    capacity_ok = true;
                    shm->active_words_on_floor[a_floor]++;
                }
            }
            else if (shm->active_words_on_floor[a_floor] < shm->max_words_per_floor &&
                     shm->active_words_on_floor[s_floor] < shm->max_words_per_floor)
            {
                capacity_ok = true;
                shm->active_words_on_floor[a_floor]++;
                shm->active_words_on_floor[s_floor]++;
            }

            if (capacity_ok)
            {
                w->arrival_floor = a_floor;
                w->admitted = 1;
                printf("[PID:%d] Word-carrier-process claimed word %d\n", getpid(), w->word_id);
                printf("[PID:%d] Word %d admitted to floor %d (sorting floor: %d)\n", getpid(), w->word_id, a_floor, s_floor);

                sem_wait(&shm->task_mutex);
                for (int j = 0; j < w->length; j++)
                {
                    if (shm->total_tasks >= MAX_TASKS)
                    {
                        fprintf(stderr, "[PID:%d] ERROR: Task queue full.\n", getpid());
                        shm->terminate_flag = 1;
                        break;
                    }
                    int t_idx = shm->total_tasks++;
                    shm->tasks[t_idx].word_index = selected_word_index;
                    shm->tasks[t_idx].character = w->original_word[j];
                    shm->tasks[t_idx].original_index = j;
                    shm->tasks[t_idx].source_floor = a_floor;
                    shm->tasks[t_idx].dest_floor = s_floor;
                    shm->tasks[t_idx].is_claimed = 0;
                    shm->tasks[t_idx].is_delivered = 0;
                }
                sem_post(&shm->task_mutex);
            }
            else
            {
                capacity_failed = true;
                w->claimed = 0;
                sem_wait(&shm->stats_mutex);
                shm->total_retries++;
                sem_post(&shm->stats_mutex);
            }
        }
        sem_post(&shm->admission_mutex);

        if (selected_word_index == -1)
        {
            usleep(IDLE_SLEEP_USEC);
        }
        else if (capacity_failed)
        {
            usleep(IDLE_SLEEP_USEC);
        }
        else
        {
            usleep(ACTIVE_SLEEP_USEC);
        }
    }
    exit(0);
}

static void letter_carrier_process(int initial_floor)
{
    srand((unsigned int)(getpid() ^ time(NULL)));
    int current_floor = initial_floor;
    printf("[PID:%d] Letter-carrier-process initialized on floor %d\n", getpid(), current_floor);

    while (!shm->terminate_flag)
    {
        int selected_task_idx = -1;
        sem_wait(&shm->task_mutex);
        for (int i = 0; i < shm->total_tasks; i++)
        {
            if (shm->tasks[i].source_floor == current_floor && shm->tasks[i].is_claimed == 0)
            {
                shm->tasks[i].is_claimed = 1;
                selected_task_idx = i;
                break;
            }
        }
        sem_post(&shm->task_mutex);

        if (selected_task_idx != -1)
        {
            CharTask *task = &shm->tasks[selected_task_idx];
            int dest = task->dest_floor;
            int word_idx = task->word_index;
            char c = task->character;

            printf("[PID:%d] Letter-carrier selected char '%c' of word %d from floor %d\n",
                   getpid(), c, shm->words[word_idx].word_id, current_floor);

            if (current_floor == dest)
            {
                printf("[PID:%d] Destination is same floor direct placement\n", getpid());
            }
            else
            {
                printf("[PID:%d] Letter-carrier requested delivery elevator from floor %d to floor %d\n",
                       getpid(), current_floor, dest);

                sem_wait(&shm->delivery_mutex);
                shm->waiting_at_floor[current_floor]++;
                sem_post(&shm->delivery_mutex);

                sem_wait(&shm->floor_wait_delivery[current_floor]);

                sem_wait(&shm->delivery_mutex);
                shm->dropping_at_floor[dest]++;
                sem_post(&shm->delivery_mutex);

                sem_wait(&shm->ride_wait_delivery[dest]);
                current_floor = dest;
            }

            sem_wait(&shm->words[word_idx].word_mutex);
            for (int j = 0; j < shm->words[word_idx].length; j++)
            {
                if (shm->words[word_idx].fixed[j] == 0 && shm->words[word_idx].occupied[j] == 0)
                {
                    shm->words[word_idx].sorting_area[j] = c;
                    shm->words[word_idx].occupied[j] = 1;
                    break;
                }
            }
            sem_post(&shm->words[word_idx].word_mutex);

            printf("[PID:%d] Letter-carrier brought char '%c' of word %d to floor %d\n",
                   getpid(), c, shm->words[word_idx].word_id, current_floor);

            sem_wait(&shm->task_mutex);
            task->is_delivered = 1;
            sem_post(&shm->task_mutex);

            sem_wait(&shm->stats_mutex);
            shm->total_chars_transported++;
            sem_post(&shm->stats_mutex);
            usleep(ACTIVE_SLEEP_USEC);
        }
        else
        {
            printf("[PID:%d] Letter-carrier found no available task on floor %d\n", getpid(), current_floor);
            printf("[PID:%d] Letter-carrier requested reposition elevator from floor %d\n", getpid(), current_floor);

            sem_wait(&shm->reposition_mutex);
            shm->waiting_reposition_at_floor[current_floor]++;
            sem_post(&shm->reposition_mutex);

            sem_wait(&shm->floor_wait_reposition[current_floor]);
            current_floor = rand() % shm->num_floors;
            usleep(IDLE_SLEEP_USEC);
        }
    }
    exit(0);
}

static void sorting_process(int floor)
{
    printf("[PID:%d] Sorting-process initialized on floor %d\n", getpid(), floor);

    while (!shm->terminate_flag)
    {
        bool worked_on_something = false;
        for (int i = 0; i < shm->total_words_in_file; i++)
        {
            Word *w = &shm->words[i];
            if (w->admitted == 1 && w->sorting_floor == floor && w->completed == 0)
            {
                if (sem_trywait(&w->word_mutex) == 0)
                {
                    bool action_taken = false;
                    for (int j = 0; j < w->length; j++)
                    {
                        if (w->occupied[j] == 1 && w->fixed[j] == 0)
                        {
                            char current_char = w->sorting_area[j];
                            int correct_pos = -1;
                            for (int k = 0; k < w->length; k++)
                            {
                                if (w->original_word[k] == current_char && w->fixed[k] == 0)
                                {
                                    correct_pos = k;
                                    break;
                                }
                            }
                            if (correct_pos != -1)
                            {
                                if (correct_pos == j)
                                {
                                    w->fixed[j] = 1;
                                    action_taken = true;
                                    printf("[PID:%d] Sorting-process fixed char '%c' of word %d on floor %d\n",
                                           getpid(), current_char, w->word_id, floor);
                                }
                                else if (w->occupied[correct_pos] == 0)
                                {
                                    w->sorting_area[correct_pos] = current_char;
                                    w->occupied[correct_pos] = 1;
                                    w->sorting_area[j] = '\0';
                                    w->occupied[j] = 0;
                                    action_taken = true;
                                    printf("[PID:%d] Sorting-process moved char '%c' to correct index for word %d\n",
                                           getpid(), current_char, w->word_id);
                                }
                                else if (w->occupied[correct_pos] == 1 && w->fixed[correct_pos] == 0)
                                {
                                    char temp = w->sorting_area[correct_pos];
                                    w->sorting_area[correct_pos] = current_char;
                                    w->sorting_area[j] = temp;
                                    action_taken = true;
                                    printf("[PID:%d] Sorting-process swapped char '%c' with '%c' for word %d\n",
                                           getpid(), current_char, temp, w->word_id);
                                }
                            }
                        }
                    }

                    bool all_fixed = true;
                    for (int j = 0; j < w->length; j++)
                    {
                        if (w->fixed[j] == 0)
                        {
                            all_fixed = false;
                            break;
                        }
                    }
                    if (all_fixed && w->length > 0)
                    {
                        w->completed = 1;
                        action_taken = true;
                        printf("[PID:%d] Word %d COMPLETED\n", getpid(), w->word_id);

                        sem_wait(&shm->admission_mutex);
                        if (w->arrival_floor >= 0)
                        {
                            shm->active_words_on_floor[w->arrival_floor]--;
                        }
                        if (w->sorting_floor != w->arrival_floor)
                        {
                            shm->active_words_on_floor[w->sorting_floor]--;
                        }
                        sem_post(&shm->admission_mutex);

                        sem_wait(&shm->stats_mutex);
                        shm->total_completed_words++;
                        sem_post(&shm->stats_mutex);
                    }
                    sem_post(&w->word_mutex);
                    if (action_taken)
                    {
                        worked_on_something = true;
                    }
                }
            }
        }
        usleep(worked_on_something ? ACTIVE_SLEEP_USEC : IDLE_SLEEP_USEC);
    }
    exit(0);
}

static void delivery_elevator_process(int capacity)
{
    int current_floor = 0;
    int direction = 1;
    int passengers_inside = 0;
    printf("[PID:%d] Delivery elevator process started\n", getpid());

    while (!shm->terminate_flag)
    {
        sem_wait(&shm->delivery_mutex);

        while (shm->dropping_at_floor[current_floor] > 0)
        {
            sem_post(&shm->ride_wait_delivery[current_floor]);
            shm->dropping_at_floor[current_floor]--;
            if (passengers_inside > 0)
            {
                passengers_inside--;
            }
        }

        while (shm->waiting_at_floor[current_floor] > 0 && passengers_inside < capacity)
        {
            sem_post(&shm->floor_wait_delivery[current_floor]);
            shm->waiting_at_floor[current_floor]--;
            passengers_inside++;
        }

        if (current_floor == shm->num_floors - 1 && direction == 1)
        {
            direction = -1;
        }
        else if (current_floor == 0 && direction == -1)
        {
            direction = 1;
        }

        if (direction != 0)
        {
            current_floor += direction;
        }

        shm->delivery_elevator_floor = current_floor;
        shm->delivery_elevator_dir = direction;
        shm->delivery_elevator_count++;
        sem_post(&shm->delivery_mutex);
        usleep(100000);
    }
    exit(0);
}

static void reposition_elevator_process(int capacity)
{
    int current_floor = 0;
    int direction = 1;
    printf("[PID:%d] Reposition elevator process started\n", getpid());

    while (!shm->terminate_flag)
    {
        sem_wait(&shm->reposition_mutex);

        int boarded = 0;
        while (shm->waiting_reposition_at_floor[current_floor] > 0 && boarded < capacity)
        {
            sem_post(&shm->floor_wait_reposition[current_floor]);
            shm->waiting_reposition_at_floor[current_floor]--;
            boarded++;
        }

        if (current_floor == shm->num_floors - 1 && direction == 1)
        {
            direction = -1;
        }
        else if (current_floor == 0 && direction == -1)
        {
            direction = 1;
        }

        if (direction != 0)
        {
            current_floor += direction;
        }

        shm->reposition_elevator_floor = current_floor;
        shm->reposition_elevator_dir = direction;
        shm->reposition_elevator_count++;
        sem_post(&shm->reposition_mutex);
        usleep(100000);
    }
    exit(0);
}

static int compare_words(const void *a, const void *b)
{
    const Word *w1 = (const Word *)a;
    const Word *w2 = (const Word *)b;
    if (w1->sorting_floor != w2->sorting_floor)
    {
        return w1->sorting_floor - w2->sorting_floor;
    }
    return w1->word_id - w2->word_id;
}

int main(int argc, char *argv[])
{
    int opt;
    int num_floors = 0, w_carriers = 0, l_carriers = 0, s_procs = 0;
    int floor_cap = 0, d_cap = 0, r_cap = 0;
    char *input_file = NULL;

    while ((opt = getopt(argc, argv, "f:w:l:s:c:d:r:i:o:")) != -1)
    {
        switch (opt)
        {
        case 'f':
            num_floors = atoi(optarg);
            break;
        case 'w':
            w_carriers = atoi(optarg);
            break;
        case 'l':
            l_carriers = atoi(optarg);
            break;
        case 's':
            s_procs = atoi(optarg);
            break;
        case 'c':
            floor_cap = atoi(optarg);
            break;
        case 'd':
            d_cap = atoi(optarg);
            break;
        case 'r':
            r_cap = atoi(optarg);
            break;
        case 'i':
            input_file = optarg;
            break;
        case 'o':
            output_file_global = optarg;
            break;
        default:
            fprintf(stderr, "Error: Invalid parameters.\n");
            return 1;
        }
    }

    if (num_floors < 1 || w_carriers < 1 || l_carriers < 1 || s_procs < 1 ||
        floor_cap < 1 || d_cap < 1 || r_cap < 1 || !input_file || !output_file_global)
    {
        fprintf(stderr, "Error: Invalid parameters.\n");
        return 1;
    }

    truncate_output_file();
    printf("Program is starting...\n");

    shm = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED)
    {
        perror("mmap failed");
        return 1;
    }
    memset(shm, 0, sizeof(SharedMemory));

    shm->num_floors = num_floors;
    shm->max_words_per_floor = floor_cap;

    for (int i = 0; i < num_floors; i++)
    {
        sem_init(&shm->floor_wait_delivery[i], 1, 0);
        sem_init(&shm->ride_wait_delivery[i], 1, 0);
        sem_init(&shm->floor_wait_reposition[i], 1, 0);
    }

    sem_init(&shm->admission_mutex, 1, 1);
    sem_init(&shm->task_mutex, 1, 1);
    sem_init(&shm->delivery_mutex, 1, 1);
    sem_init(&shm->reposition_mutex, 1, 1);
    sem_init(&shm->stats_mutex, 1, 1);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    printf("Input file is being read...\n");
    FILE *in_fp = fopen(input_file, "r");
    if (!in_fp)
    {
        fprintf(stderr, "Error: Input file '%s' not found or cannot be opened.\n", input_file);
        cleanup_and_exit(EXIT_FAILURE);
    }

    int id, sf;
    char word[MAX_WORD_LEN];
    while (fscanf(in_fp, "%d %63s %d", &id, word, &sf) == 3 && shm->total_words_in_file < MAX_WORDS)
    {
        input_line_count++;

        if (sf < 0 || sf >= shm->num_floors)
        {
            fprintf(stderr, "Warning: Word %d has invalid sorting floor %d. Ignored.\n", id, sf);
            continue;
        }
        if (!is_valid_lowercase_word(word))
        {
            fprintf(stderr, "Warning: Word %d ('%s') contains invalid characters. Only lowercase english letters are allowed. Ignored.\n",
                    id, word);
            continue;
        }

        Word *w = &shm->words[shm->total_words_in_file];
        w->word_id = id;
        snprintf(w->original_word, MAX_WORD_LEN, "%s", word);
        w->length = (int)strlen(word);
        w->sorting_floor = sf;
        w->arrival_floor = -1;
        sem_init(&w->word_mutex, 1, 1);
        shm->total_words_in_file++;
    }
    fclose(in_fp);

    if (input_line_count == 0)
    {
        printf("Empty input file detected. Empty output file created.\n");
        cleanup_and_exit(EXIT_SUCCESS);
    }

    if (shm->total_words_in_file == 0)
    {
        fprintf(stderr, "Error: No valid words found to process in the input file. Terminating.\n");
        cleanup_and_exit(EXIT_FAILURE);
    }

    printf("Shared memory is initialized...\n");
    printf("Synchronization primitives are created...\n");
    printf("Processes are being created...\n");
    printf("[PID:%d] Parent process started\n", getpid());

    for (int f = 0; f < num_floors; f++)
    {
        printf("Initializing Floor %d\n", f);
        for (int i = 0; i < w_carriers; i++)
        {
            if (fork() == 0)
            {
                word_carrier_process(f);
            }
        }
        for (int i = 0; i < l_carriers; i++)
        {
            if (fork() == 0)
            {
                letter_carrier_process(f);
            }
        }
        for (int i = 0; i < s_procs; i++)
        {
            if (fork() == 0)
            {
                sorting_process(f);
            }
        }
    }

    if (fork() == 0)
    {
        delivery_elevator_process(d_cap);
    }
    if (fork() == 0)
    {
        reposition_elevator_process(r_cap);
    }

    while (!shm->terminate_flag)
    {
        bool all_done = !any_incomplete_words() && !any_pending_tasks();
        if (all_done && shm->total_words_in_file > 0)
        {
            printf("All words have been transported and sorted...\n");
            shm->terminate_flag = 1;
        }
        if (!shm->terminate_flag)
        {
            usleep(100000);
        }
    }

    signal(SIGTERM, SIG_IGN);
    kill(0, SIGTERM);
    while (waitpid(-1, NULL, 0) > 0)
    {
    }

    if (!sigint_received)
    {
        printf("Output file is being created...\n");
        qsort(shm->words, shm->total_words_in_file, sizeof(Word), compare_words);

        FILE *out_fp = fopen(output_file_global, "w");
        if (out_fp)
        {
            for (int i = 0; i < shm->total_words_in_file; i++)
            {
                shm->words[i].sorting_area[shm->words[i].length] = '\0';
                fprintf(out_fp, "%d %s %d\n", shm->words[i].word_id,
                        shm->words[i].sorting_area, shm->words[i].sorting_floor);
            }
            fclose(out_fp);
        }

        printf("\nSystem Summary:\n");
        printf("Total words: %d\n", shm->total_words_in_file);
        printf("Completed words: %d\n", shm->total_completed_words);
        printf("Retries: %d\n", shm->total_retries);
        printf("Characters transported: %d\n", shm->total_chars_transported);
        printf("Delivery elevator operations: %d\n", shm->delivery_elevator_count);
        printf("Reposition elevator operations: %d\n", shm->reposition_elevator_count);
        printf("Program terminated successfully.\n");
    }
    else
    {
        printf("\nProgram terminated by user (Ctrl+C). Resources cleaned up.\n");
    }

    cleanup_and_exit(0);
    return 0;
}
