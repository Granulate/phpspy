#include "phpspy.h"
#include <poll.h>
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <ctype.h>

static int wait_for_turn(char producer_or_consumer);
static void pgrep_for_pids();
static void *run_work_thread(void *arg);
static int is_already_attached(int pid);
static void init_work_threads();
static void deinit_work_threads();
static int block_all_signals();
static void handle_signal(int signum);
static void *run_signal_thread(void *arg);
static int init_pgrep_output_pipe(void);
static int install_sigusr2_handler(void);
static void handle_sigusr2(int signal);
static int fetch_current_timestamp(char *buf, size_t sz);
static void *run_write_output_thread(void *arg);
static int drain_pipe_to_file(int check_for_done);
static int get_php_procs(pid_t procs_arr[], size_t procs_size);
static int read_file(const char *path, void *buf, size_t size);
static int is_number(const char *str);
static int try_find_match_in_proc_fs(pid_t pid, const char *proc_fs_f_name);

static int *avail_pids = NULL;
static int *attached_pids = NULL;
static pthread_t *work_threads = NULL;
static pthread_t signal_thread;
static pthread_t rotate_output_thread;
static int avail_pids_count = 0;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t can_produce = PTHREAD_COND_INITIALIZER;
static pthread_cond_t can_consume = PTHREAD_COND_INITIALIZER;
static int should_rotate = 0;
static int done_pipe[2] = { -1, -1 };

/*
    A little brief on how the output works in pgrep mode:
    In order to prevent interlaced outputs in large buffers (bigger than PIPE_BUF), we do the following:

    A buffer (udata->buf) is allocated on each stack begin, frames of the same stack are being written to this buffer,
    When the stack finishes, the buffer and the size of the written data are written to a pipe (unlike the normal PID mode where the buffer
    is written directly to stdout), because the data and the size (write_msg_t) is a lot smaller than PIPE_BUF operations are synchronized.
    On a polling thread, we poll on the the read side of the pipe where we receive messages (write_msg_t), we read these messages
    and "drain" them to the output file, after writing the data we free the buffer that was previously allocated.
    The reason we are fully secured is that only a single thread writes to the outputfile in pgrep mode.
*/
static int output_pipe[2] = { -1, -1 };

int main_pgrep() {
    long i;

    if (opt_num_workers < 1) {
        log_error("Expected max concurrent workers (-T) > 0\n");
        exit(1);
    }

    if (init_output_fd() != PHPSPY_OK) {
        exit(1);
    }

    init_pgrep_output_pipe();
    pthread_create(&rotate_output_thread, NULL, run_write_output_thread, NULL);
    install_sigusr2_handler();

    pthread_create(&signal_thread, NULL, run_signal_thread, NULL);
    block_all_signals();

    init_work_threads();

    for (i = 0; i < opt_num_workers; i++) {
        pthread_create(&work_threads[i], NULL, run_work_thread, (void*)i);
    }

    if (opt_time_limit_ms > 0) {
        alarm(PHPSPY_MAX(1, opt_time_limit_ms / 1000));
    }

    pgrep_for_pids();

    for (i = 0; i < opt_num_workers; i++) {
        pthread_join(work_threads[i], NULL);
    }
    pthread_join(signal_thread, NULL);
    pthread_join(rotate_output_thread, NULL);

    deinit_work_threads();

    close(output_pipe[1]); /* First, close write fd to the pipe so no more stacks will be written */
    drain_pipe_to_file(0); /* Drain remaining stacks and free the buffers */
    close(output_pipe[0]);
    deinit_output_fd();

    log_error("main_pgrep finished gracefully\n");
    return 0;
}

static int init_pgrep_output_pipe(void) {
    int rv;
    rv = pipe(output_pipe);
    if (rv != 0) {
        log_error("Couldn't open output pipe - errno %d\n");
        return PHPSPY_ERR;
    }
    /* Set read fd as non-blocking */
    fcntl(output_pipe[0], F_SETFL, fcntl(output_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    return PHPSPY_OK;
}


int pgrep_mode_output_write(const char *buf, size_t buf_size) {
    int rv = PHPSPY_OK;

    struct iovec io_vec;
    io_vec.iov_base = (void *) buf;
    io_vec.iov_len = buf_size;
    if ((rv = write(output_pipe[1], &io_vec, sizeof(io_vec)) != sizeof(io_vec))) {
        log_error("FATAL! couldn't write message in a single chunk to the pipe\n");
    }
    return rv;
}

static int wait_for_turn(char producer_or_consumer){
    struct timespec timeout;
    pthread_mutex_lock(&mutex);
    while (!done) {
        if (producer_or_consumer == 'p' && avail_pids_count < opt_num_workers) {
            break;
        } else if (avail_pids_count > 0) {
            break;
        }
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 2;
        pthread_cond_timedwait(
            producer_or_consumer == 'p' ? &can_produce : &can_consume,
            &mutex,
            &timeout
        );
    }
    if (done) {
        pthread_mutex_unlock(&mutex);
        return 1;
    }
    return 0;
}

static int read_file(const char *path, void *buf, size_t size) {
    int rv = PHPSPY_OK;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        if (errno != ENOENT) {
            perror("Can't open file for reading");
        }
        return -PHPSPY_ERR;
    }

    rv = fread(buf, 1, size, f);
    if (ferror(f)) {
        if (errno != ENOENT) {
            perror("Couldn't read data from file");
        }

        rv = -PHPSPY_ERR;
    }

    fclose(f);
    return rv;
}

static int is_number(const char *str) {
    size_t str_l = strlen(str);
    int i = 0;

    for (i = 0; i < (int) str_l; ++i) {
        if (!isdigit(str[i])) {
            return 0;
        }
    }
    return 1;
}

static int try_find_match_in_proc_fs(pid_t pid, const char *proc_fs_f_name) {
    char proc_path[PATH_MAX];
    char proc_fs_read_buf[4096] = {0};
    char *match;
    snprintf(proc_path, PATH_MAX, "/proc/%d/%s", pid, proc_fs_f_name);

    if (read_file(proc_path, proc_fs_read_buf, sizeof(proc_fs_read_buf)) < 0) {
        if (errno != ENOENT) {
            perror("Couldn't read proc fs file");
        }
        return -1;
    }
    /* Ensure null termination so libc string functions will work properly */
    proc_fs_read_buf[sizeof(proc_fs_read_buf) - 1] = '\0';

    /* Using strstr is ok also for cmdline because cmdline is null splitted between the args. */
    if ((match = strstr(proc_fs_read_buf, opt_pgrep_args)) != NULL) {
        return 0;
    }

    return 1;
}

static int get_php_procs(pid_t procs_arr[], size_t procs_size) {
    DIR *dp;
    pid_t pid;
    struct dirent *ep;
    size_t num_procs_matched = 0;

    dp = opendir("/proc");
    if (dp == NULL) {
        perror("Couldn't open '/proc' directory");
        return -PHPSPY_ERR;
    }

    while ((ep = readdir(dp)) && (num_procs_matched < procs_size)) {
        if (!is_number(ep->d_name)) {
            continue;
        }
        pid = atoi(ep->d_name);

        /* First try with comm, then fallback to "argv[0]", like pgrep default behavior */
        if (try_find_match_in_proc_fs(pid, "comm") == 0) {
            procs_arr[num_procs_matched++] = pid;
            continue;
        }

        if (try_find_match_in_proc_fs(pid, "cmdline") == 0) {
            procs_arr[num_procs_matched++] = pid;
        }
    }
    (void)closedir(dp);

    return (int) num_procs_matched;
}

static void pgrep_for_pids() {
    int pid;
    int found;
    int num_procs_matched;
    struct timespec timeout;
    int i;
    pid_t *procs_arr = malloc(sizeof(pid_t) * opt_num_workers);

    if (procs_arr == NULL) {
        log_error("FATAL: pgrep mode: can't allocate pid search buffer\n");
        return;
    }

    while (!done){
        if (wait_for_turn('p')) break;
        found = 0;
        num_procs_matched = get_php_procs(procs_arr, opt_num_workers);
        if (num_procs_matched > 0) {
            for (i = 0; i < num_procs_matched && avail_pids_count < opt_num_workers; i++) {
                pid = procs_arr[i];
                if (is_already_attached(pid))
                    continue;
                avail_pids[avail_pids_count++] = pid;
                found += 1;
            }
        }

        if (found > 0) {
            pthread_cond_broadcast(&can_consume);
        } else {
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += 2;
            pthread_cond_timedwait(
                &can_produce,
                &mutex,
                &timeout
            );
        }
        pthread_mutex_unlock(&mutex);
    }
    free(procs_arr);
}

static void *run_work_thread(void *arg) {
    int worker_num;
    int main_pid_rv;
    pid_t pid;
    worker_num = (long)arg;
    while (!done) {
        if (wait_for_turn('c')) break;
        attached_pids[worker_num] = avail_pids[--avail_pids_count];
        pthread_cond_signal(&can_produce);
        pthread_mutex_unlock(&mutex);
        pid = attached_pids[worker_num];
        main_pid_rv = main_pid(pid);

        /* PHPSPY_ERR_PID_DEAD is sometimes passed ORred with other error(s) */
        if (main_pid_rv != PHPSPY_OK && (main_pid_rv & PHPSPY_ERR_PID_DEAD) == 0) {
            log_error("error: pgrep mode: main_pid routine returned non ok status and PID (%d) is not dead, rv = %d, errno - %d\n",
                      pid, main_pid_rv, errno);
        }

        attached_pids[worker_num] = 0;
    }
    return NULL;
}

static int is_already_attached(int pid) {
    int i;
    for (i = 0; i < opt_num_workers; i++) {
        if (attached_pids[i] == pid) {
            return 1;
        } else if (i < avail_pids_count && avail_pids[i] == pid) {
            return 1;
        }
    }
    return 0;
}

static void init_work_threads() {
    avail_pids = calloc(opt_num_workers, sizeof(int));
    attached_pids = calloc(opt_num_workers, sizeof(int));
    work_threads = calloc(opt_num_workers, sizeof(pthread_t));
    if (!avail_pids || !attached_pids || !work_threads) {
        errno = ENOMEM;
        perror("calloc");
        exit(1);
    }
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&can_produce, NULL);
    pthread_cond_init(&can_consume, NULL);
}

static void deinit_work_threads() {
    free(avail_pids);
    free(attached_pids);
    free(work_threads);
    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&can_produce);
    pthread_cond_destroy(&can_consume);
}

static int block_all_signals() {
    int rv;
    sigset_t set;
    try(rv, sigfillset(&set));
    try(rv, sigprocmask(SIG_BLOCK, &set, NULL));
    return 0;
}

void write_done_pipe() {
    int rv, ignore;
    if (done_pipe[1] >= 0) {
        ignore = 1;
        rv = write(done_pipe[1], &ignore, sizeof(int));
    }
    (void)rv;
}

static void handle_signal(int signum) {
    (void)signum;
    write_done_pipe();
}

static void *run_signal_thread(void *arg) {
    int rv, ignore;
    fd_set rfds;
    struct timeval tv;
    struct sigaction sa;

    (void)arg;

    /* Create done_pipe */
    rv = pipe(done_pipe);
    rv = fcntl(done_pipe[1], F_SETFL, O_NONBLOCK);

    /* Install signal handler */
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    /* Wait for write on done_pipe from write_done_pipe */
    do {
        FD_ZERO(&rfds);
        FD_SET(done_pipe[0], &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        rv = select(done_pipe[0]+1, &rfds, NULL, NULL, &tv);
    } while (rv < 1);

    /* Read pipe for fun */
    rv = read(done_pipe[0], &ignore, sizeof(int));

    /* Set done flag; wake up all threads */
    done = 1;
    pthread_mutex_lock(&mutex);
    pthread_cond_broadcast(&can_consume);
    pthread_cond_broadcast(&can_produce);
    pthread_mutex_unlock(&mutex);

    return NULL;
}

static int install_sigusr2_handler(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_handler = handle_sigusr2;
    sigaction(SIGUSR2, &sa, NULL);

    return PHPSPY_OK;
}

/* Copied from linux: tools/perf/util/time-utils.c */
static int fetch_current_timestamp(char *buf, size_t sz) {
    struct timeval tv;
    struct tm tm;
    char dt[32];

    if (gettimeofday(&tv, NULL) || !localtime_r(&tv.tv_sec, &tm))
        return -1;

    if (!strftime(dt, sizeof(dt), "%Y%m%d%H%M%S", &tm))
        return -1;

    snprintf(buf, sz, "%s%02u", dt, (unsigned)tv.tv_usec / 10000);

    return 0;
}

static void handle_sigusr2(int signal) {
    (void)signal;
    should_rotate = 1;
}

static void rotate_output(void) {
#define TIMESTAMP_BUF_SIZE (128)
    char timestamp_buf[TIMESTAMP_BUF_SIZE];
    char new_path[PATH_MAX];

    if (strcmp(opt_path_output, STDOUT_OUTPUT) == 0) {
        return;
    }

    deinit_output_fd();

    if (fetch_current_timestamp(timestamp_buf, TIMESTAMP_BUF_SIZE) != 0) {
        strncpy(timestamp_buf, "UNKNOWN-TS", TIMESTAMP_BUF_SIZE);
    }
    snprintf(new_path, PATH_MAX, "%s.%s", opt_path_output, timestamp_buf);
    rename(opt_path_output, new_path);

    if (init_output_fd() != PHPSPY_OK) {
        log_error("FATAL! couldn't reopen log output file after rotation\n");
        /* flag to exit ASAP */
        done = 1;
    }
}

static int drain_pipe_to_file(int check_for_done) {
    int rv = 0;

    struct iovec io_vec;
    while ((rv = read(output_pipe[0], &io_vec, sizeof(io_vec))) != -1 && !should_rotate && (!check_for_done || !done)) {
        if (rv == 0) { /* EOF */
            return PHPSPY_OK;
        }

        if (rv != sizeof(io_vec)) {
            log_error("FATAL! read %d bytes from the pipe which is not a full write_msg_t (sizeof = %d)\n", rv, sizeof(io_vec));
            done = 1; /* can't recover from this stage, but really should never happen */
            return PHPSPY_ERR;
        }

        if (write(output_fd, io_vec.iov_base, io_vec.iov_len) != (int)io_vec.iov_len) {
            log_error("event_handler_fout: Write failed (%s)\n", errno != 0 ? strerror(errno) : "partial");
        }
        free(io_vec.iov_base); /* Allocated at event_handler_fout at EVENT_STACK_BEGIN */
    }

    if (rv != 0 && errno != EAGAIN) {
        perror("Error while draining pipe messages to output file");
        return PHPSPY_ERR;
    }

    return PHPSPY_OK;
}

static void *run_write_output_thread(void *arg) {
    int rv;
    struct pollfd poll_pipe = {0};
    (void)arg;

    poll_pipe.fd = output_pipe[0];
    poll_pipe.events = POLLIN;

    while (!done) {
        if (should_rotate) {
            rotate_output();
            should_rotate = 0;
        }

        /* See explanation above output_pipe declaration */
#define POLL_TIMEOUT_MS (50)
        rv = poll(&poll_pipe, 1, POLL_TIMEOUT_MS);
        if (rv == 0) { /* timeout - continue */
            continue;
        } else if (rv < 0) {
            if (errno != EINTR) {
                perror("poll on pipe fd failed");
            }
            continue;
        }
        drain_pipe_to_file(1);
    }

    return NULL;
}
