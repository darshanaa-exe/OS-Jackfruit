/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 * Final Submission with Verbose Pipeline Logging
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/resource.h> 
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* --- Constants --- */
#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 4096
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

/* --- Enums and Structs --- */
typedef enum {
    CMD_SUPERVISOR = 0, CMD_START, CMD_RUN, CMD_PS, CMD_LOGS, CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0, CONTAINER_RUNNING, CONTAINER_STOPPED, CONTAINER_KILLED, CONTAINER_EXITED
} container_state_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head; size_t tail; size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} producer_ctx_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int stop_requested;
    void *stack_ptr; 
    struct container_record *next;
} container_record_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* --- Globals & Signals --- */
static volatile sig_atomic_t pending_reap = 0;
static volatile sig_atomic_t shutdown_requested = 0;

static void sigchld_handler(int s) { (void)s; pending_reap = 1; }
static void sigterm_handler(int s) { (void)s; shutdown_requested = 1; }

static const char *state_to_string(container_state_t state) {
    switch (state) {
        case CONTAINER_RUNNING: return "running";
        case CONTAINER_STOPPED: return "stopped";
        case CONTAINER_KILLED:  return "hard_limit_killed";
        case CONTAINER_EXITED:  return "exited";
        default:                return "starting";
    }
}

/* --- Bounded Buffer Logic (with Push Logs) --- */
static void bb_init(bounded_buffer_t *b) {
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
}

static int bb_push(bounded_buffer_t *b, const log_item_t *item) {
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down) 
        pthread_cond_wait(&b->not_full, &b->mutex);
    
    if (b->shutting_down) { pthread_mutex_unlock(&b->mutex); return -1; }
    
    b->items[b->tail] = *item; 
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY; 
    b->count++;
    
    printf("[BUFFER PUSH] container=%s\n", item->container_id);
    fflush(stdout);

    pthread_cond_signal(&b->not_empty); 
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

static int bb_pop(bounded_buffer_t *b, log_item_t *item) {
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0 && !b->shutting_down) pthread_cond_wait(&b->not_empty, &b->mutex);
    if (b->shutting_down && b->count == 0) { pthread_mutex_unlock(&b->mutex); return -1; }
    *item = b->items[b->head]; b->head = (b->head + 1) % LOG_BUFFER_CAPACITY; b->count--;
    pthread_cond_signal(&b->not_full); pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* --- Logging Thread (Consumer) --- */
void *logging_thread(void *arg) {
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item; mkdir(LOG_DIR, 0777);
    while (bb_pop(&ctx->log_buffer, &item) == 0) {
        printf("[CONSUMER] writing logs for %s\n", item.container_id);
        fflush(stdout);

        char p[PATH_MAX]; snprintf(p, sizeof(p), "%s/%s.log", LOG_DIR, item.container_id);
        int fd = open(p, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) { if(write(fd, item.data, item.length)); close(fd); }
    }
    return NULL;
}

/* --- Producer Thread --- */
void *producer_thread(void *arg) {
    producer_ctx_t *pctx = (producer_ctx_t *)arg;
    log_item_t item; strncpy(item.container_id, pctx->container_id, CONTAINER_ID_LEN);
    while (1) {
        ssize_t b = read(pctx->read_fd, item.data, LOG_CHUNK_SIZE - 1);
        if (b <= 0) break;
        
        printf("[PRODUCER] container=%s active, last read=%ld bytes\n", item.container_id, b);
        fflush(stdout);

        item.length = b; item.data[b] = '\0';
        if (bb_push(pctx->log_buffer, &item) != 0) break;
    }
    close(pctx->read_fd); free(pctx); return NULL;
}

/* --- Container Child --- */
int child_fn(void *arg) {
    child_config_t *c = (child_config_t *)arg;
    sethostname(c->id, strlen(c->id));
    setpriority(PRIO_PROCESS, 0, c->nice_value);
    if (chroot(c->rootfs) != 0 || chdir("/") != 0) return 1;
    mount("proc", "/proc", "proc", 0, NULL);
    dup2(c->log_write_fd, 1); dup2(c->log_write_fd, 2); close(c->log_write_fd);
    char *args[] = { "/bin/sh", "-c", c->command, NULL };
    execvp(args[0], args);
    return 1;
}

/* --- Supervisor Logic (Main Loop & Reaper) --- */
static int run_supervisor(const char *base_rootfs) {
    supervisor_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.metadata_lock, NULL); bb_init(&ctx.log_buffer);
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    bind(ctx.server_fd, (struct sockaddr*)&addr, sizeof(addr)); listen(ctx.server_fd, 10);
    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = sigchld_handler; sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = sigterm_handler; sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    printf("Supervisor active. Base: %s\n", base_rootfs);

    while (!shutdown_requested) {
        if (pending_reap) {
            pending_reap = 0; int status; pid_t pid;
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                pthread_mutex_lock(&ctx.metadata_lock);
                container_record_t *curr = ctx.containers;
                while (curr) {
                    if (curr->host_pid == pid) {
                        if (WIFEXITED(status)) {
                            curr->state = CONTAINER_EXITED;
                        } else if (WIFSIGNALED(status)) {
                            if (curr->stop_requested) {
                                curr->state = CONTAINER_STOPPED;
                            } else {
                                curr->state = CONTAINER_KILLED;
                                printf("[HARD LIMIT] container=%s exceeded hard limit -> killing\n", curr->id);
                                printf("[KILLED] container=%s PID=%d\n", curr->id, pid);
                                fflush(stdout);
                            }
                        }
                        struct monitor_request m = { .pid = pid }; strncpy(m.container_id, curr->id, 31);
                        ioctl(ctx.monitor_fd, MONITOR_UNREGISTER, &m); break;
                    }
                    curr = curr->next;
                }
                pthread_mutex_unlock(&ctx.metadata_lock);
            }
        }

        struct timeval tv = { .tv_sec = 1 };
        fd_set fds; FD_ZERO(&fds); FD_SET(ctx.server_fd, &fds);
        if (select(ctx.server_fd + 1, &fds, NULL, NULL, &tv) <= 0) continue;

        int cfd = accept(ctx.server_fd, NULL, NULL);
        control_request_t req;
        if (cfd >= 0 && recv(cfd, &req, sizeof(req), 0) > 0) {
            control_response_t res = { .status = 0 };
            if (req.kind == CMD_START || req.kind == CMD_RUN) {
                int p[2]; if(pipe(p));
                child_config_t *cc = malloc(sizeof(child_config_t));
                strncpy(cc->id, req.container_id, 31); strncpy(cc->rootfs, req.rootfs, PATH_MAX-1);
                strncpy(cc->command, req.command, 255); cc->log_write_fd = p[1]; cc->nice_value = req.nice_value;
                void *stack = malloc(STACK_SIZE);
                
                printf("Starting container: %s\n", cc->id);
                pid_t pid = clone(child_fn, (char *)stack + STACK_SIZE, CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, cc);
                if (pid > 0) {
                    close(p[1]);
                    container_record_t *r = malloc(sizeof(container_record_t)); memset(r, 0, sizeof(*r));
                    strncpy(r->id, req.container_id, 31); r->host_pid = pid; r->state = CONTAINER_RUNNING;
                    r->soft_limit_bytes = req.soft_limit_bytes; r->hard_limit_bytes = req.hard_limit_bytes; 
                    time(&r->started_at); r->stack_ptr = stack; 
                    pthread_mutex_lock(&ctx.metadata_lock); r->next = ctx.containers; ctx.containers = r; pthread_mutex_unlock(&ctx.metadata_lock);
                    
                    producer_ctx_t *pctx = malloc(sizeof(producer_ctx_t));
                    pctx->read_fd = p[0]; strncpy(pctx->container_id, req.container_id, 31); pctx->log_buffer = &ctx.log_buffer;
                    pthread_t pt; pthread_create(&pt, NULL, producer_thread, pctx); pthread_detach(pt);

                    struct monitor_request m = { .pid = pid, .soft_limit_bytes = r->soft_limit_bytes, .hard_limit_bytes = r->hard_limit_bytes };
                    strncpy(m.container_id, r->id, 31); ioctl(ctx.monitor_fd, MONITOR_REGISTER, &m);
                    
                    printf("Container %s started with PID %d\n", r->id, pid);
                    fflush(stdout);
                    snprintf(res.message, 4095, "Started %s (PID %d)", r->id, pid);
                }
            } else if (req.kind == CMD_PS) {
                char buf[4096] = "ID\t\tPID\tSTATE\t\tTIME\t\tSOFT\tHARD\n----------------------------------------------------------------------------\n";
                pthread_mutex_lock(&ctx.metadata_lock);
                container_record_t *curr = ctx.containers;
                while (curr) {
                    struct tm *t = localtime(&curr->started_at); char ts[10]; strftime(ts, 10, "%H:%M:%S", t);
                    char line[512]; snprintf(line, 511, "%s\t\t%d\t%s\t\t%s\t%lu\t%lu\n", curr->id, curr->host_pid, state_to_string(curr->state), ts, curr->soft_limit_bytes/(1UL<<20), curr->hard_limit_bytes/(1UL<<20));
                    strncat(buf, line, 4095 - strlen(buf)); curr = curr->next;
                }
                pthread_mutex_unlock(&ctx.metadata_lock); strncpy(res.message, buf, 4095);
            } else if (req.kind == CMD_STOP) {
                pthread_mutex_lock(&ctx.metadata_lock);
                container_record_t *curr = ctx.containers;
                while (curr) {
                    if (strcmp(curr->id, req.container_id) == 0 && curr->state == CONTAINER_RUNNING) {
                        curr->stop_requested = 1; kill(curr->host_pid, SIGKILL);
                        snprintf(res.message, 4095, "Stopping %s...", curr->id); break;
                    }
                    curr = curr->next;
                }
                pthread_mutex_unlock(&ctx.metadata_lock);
            } else if (req.kind == CMD_LOGS) {
                char path[PATH_MAX]; snprintf(path, PATH_MAX-1, "logs/%s.log", req.container_id);
                int fd = open(path, O_RDONLY);
                if (fd >= 0) { ssize_t b = read(fd, res.message, 4094); if (b>=0) res.message[b] = '\0'; close(fd); }
                else strcpy(res.message, "No logs found.");
            }
            send(cfd, &res, sizeof(res), 0);
            close(cfd);
        }
    }

    pthread_mutex_lock(&ctx.log_buffer.mutex); ctx.log_buffer.shutting_down = 1; 
    pthread_cond_broadcast(&ctx.log_buffer.not_empty); pthread_mutex_unlock(&ctx.log_buffer.mutex);
    pthread_join(ctx.logger_thread, NULL);

    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *it = ctx.containers;
    while (it) {
        container_record_t *tmp = it; it = it->next;
        if (tmp->state == CONTAINER_RUNNING) kill(tmp->host_pid, SIGKILL);
        free(tmp->stack_ptr); free(tmp);
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    close(ctx.server_fd); close(ctx.monitor_fd); unlink(CONTROL_PATH);
    return 0;
}

/* --- CLI Main --- */
int main(int argc, char *argv[]) {
    if (argc < 2) return 1;
    if (strcmp(argv[1], "supervisor") == 0) return run_supervisor(argv[2]);
    
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); 
    struct sockaddr_un a = { .sun_family = AF_UNIX }; 
    strncpy(a.sun_path, CONTROL_PATH, sizeof(a.sun_path)-1);
    
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) < 0) {
        perror("Failed to connect to supervisor socket");
        return 1;
    }
    
    control_request_t req; memset(&req, 0, sizeof(req));
    if (strcmp(argv[1], "start") == 0 || strcmp(argv[1], "run") == 0) {
        req.kind = CMD_START; 
        strncpy(req.container_id, argv[2], 31); 
        strncpy(req.rootfs, argv[3], PATH_MAX-1);
        strncpy(req.command, argv[4], 255); 
        req.soft_limit_bytes = DEFAULT_SOFT_LIMIT; 
        req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
        
        for (int i=5; i<argc; i+=2) {
            if (i+1>=argc) break;
            if (strcmp(argv[i], "--soft-mib") == 0) req.soft_limit_bytes = strtoul(argv[i+1], NULL, 10)*(1UL<<20);
            else if (strcmp(argv[i], "--hard-mib") == 0) req.hard_limit_bytes = strtoul(argv[i+1], NULL, 10)*(1UL<<20);
            else if (strcmp(argv[i], "--nice") == 0) req.nice_value = atoi(argv[i+1]);
        }
    } else if (strcmp(argv[1], "ps") == 0) req.kind = CMD_PS;
    else if (strcmp(argv[1], "stop") == 0) { req.kind = CMD_STOP; strncpy(req.container_id, argv[2], 31); }
    else if (strcmp(argv[1], "logs") == 0) { req.kind = CMD_LOGS; strncpy(req.container_id, argv[2], 31); }
    
    send(fd, &req, sizeof(req), 0); 
    control_response_t res;
    if (recv(fd, &res, sizeof(res), 0) > 0) printf("%s\n", res.message);
    close(fd); return 0;
}
