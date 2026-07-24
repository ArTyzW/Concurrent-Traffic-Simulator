// garage.c — spawner (no polling) with ordered batches, serialized logging
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <semaphore.h>

#include "incrocio.h"   // NUM_STRADE, EstraiDirezione

// ---- ABI/shared names with incrocio ----
typedef struct {
    pid_t pidauto;
    int   provenienza;
    int   direzione;
} AutoInfo;

#define SHM_CARS        "/shared_auto"
#define SHM_FLAG        "/flag_termina"
#define SEM_READY_RES   "/semaforo_risorse"     // garage -> incrocio: resources created
#define SEM_BATCH       "/sem_gi"               // garage -> incrocio: batch ready
#define SEM_GARAGE_DONE "/semaforo_terminale"   // garage -> incrocio: proceed to shutdown
#define SEM_NEXTBATCH   "/sem_nextbatch"        // incrocio -> garage: start next batch
#define SEM_FILE        "/sem_auto_file"        // mutex for auto.txt file
#define SEM_PRINT_SYNC  "/sem_ready"            // parent/child print sync
#define SEM_LOG         "/sem_log"              // serializes stdout printing

// ---- garage signal handling (parent) ----
// NOTE: we ignore SIGTERM (must not stop itself). Log to screen only.
static void on_sigint(int s){ (void)s;
    const char m[]="[garage] SIGINT ignorato (termina con SIGTERM su incrocio).\n";
    (void)!write(STDERR_FILENO,m,sizeof m-1);
}
static void on_sigterm(int s){ (void)s;
    const char m[]="[garage] SIGTERM ricevuto (ignorato: lo stop si comanda da incrocio).\n";
    (void)!write(STDERR_FILENO,m,sizeof m-1);
}

// ---- stop management in car processes (children) ----
static volatile sig_atomic_t stop_auto = 0;
static void on_term_auto(int s){ (void)s; stop_auto = 1; }

static sem_t* sem_open_or_die(const char *name, int oflag, mode_t mode, unsigned val) {
    sem_t *s = sem_open(name, oflag, mode, val);
    if (s == SEM_FAILED) { perror(name); exit(EXIT_FAILURE); }
    return s;
}
static int robust_sem_wait_stop(sem_t *s, volatile sig_atomic_t *stop_flag) {
    for (;;) {
        if (sem_wait(s) == 0) return 0;
        if (errno == EINTR) {
            if (stop_flag && *stop_flag) return -1;
            continue;
        }
        return -1;
    }
}
static void log_linef(sem_t *sem_log, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[256];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (sem_log) sem_wait(sem_log);
    (void)!write(STDOUT_FILENO, buf, (size_t)n);
    if (sem_log) sem_post(sem_log);
}

int main(void) {
    // --- signal handling (parent) ---
    struct sigaction sa = {0}; sa.sa_flags=0; sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_sigint;  sigaction(SIGINT,  &sa, NULL);
    sa.sa_handler = on_sigterm; sigaction(SIGTERM, &sa, NULL);

    // --- shared memory: cars ---
    int fd_cars = shm_open(SHM_CARS, O_CREAT | O_RDWR, 0666);
    if (fd_cars == -1) { perror("shm_open " SHM_CARS); exit(EXIT_FAILURE); }
    size_t cars_sz = sizeof(AutoInfo) * NUM_STRADE;
    if (ftruncate(fd_cars, cars_sz) == -1) { perror("ftruncate " SHM_CARS); exit(EXIT_FAILURE); }
    AutoInfo *cars = mmap(NULL, cars_sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd_cars, 0);
    if (cars == MAP_FAILED) { perror("mmap cars"); exit(EXIT_FAILURE); }

    // --- shared memory: run flag ---
    int fd_flag = shm_open(SHM_FLAG, O_CREAT | O_RDWR, 0666);
    if (fd_flag == -1) { perror("shm_open " SHM_FLAG); exit(EXIT_FAILURE); }
    if (ftruncate(fd_flag, sizeof(int)) == -1) { perror("ftruncate " SHM_FLAG); exit(EXIT_FAILURE); }
    int *runflag = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, fd_flag, 0);
    if (runflag == MAP_FAILED) { perror("mmap flag"); exit(EXIT_FAILURE); }
    *runflag = 1;

    // --- global semaphores (incrocio creates them, but O_CREAT is safe) ---
    sem_t *sem_ready_res   = sem_open_or_die(SEM_READY_RES,   O_CREAT, 0600, 0);
    sem_t *sem_batch       = sem_open_or_die(SEM_BATCH,       O_CREAT, 0666, 0);
    sem_t *sem_garage_done = sem_open_or_die(SEM_GARAGE_DONE, O_CREAT, 0600, 0);
    sem_t *sem_print_sync  = sem_open_or_die(SEM_PRINT_SYNC,  O_CREAT, 0666, 0);
    sem_t *sem_file_mutex  = sem_open_or_die(SEM_FILE,        O_CREAT, 0600, 1);
    sem_t *sem_log         = sem_open_or_die(SEM_LOG,         O_CREAT, 0600, 1);
    sem_t *sem_nextbatch   = sem_open_or_die(SEM_NEXTBATCH,   O_CREAT, 0600, 0); // initial value set by incrocio

    // notify incrocio that resources can be opened (SHM already created)
    sem_post(sem_ready_res);
    sem_close(sem_ready_res);

    // =================== BATCH LOOP ===================
    while (*runflag == 1) {

        // wait for green light from incrocio to start a new batch (SIGTERM here is ignored)
        robust_sem_wait_stop(sem_nextbatch, NULL);
        if (*runflag != 1) break;

        // create NUM_STRADE cars (one for each street 0..NUM_STRADE-1)
        for (int i = 0; i < NUM_STRADE; ++i) {
            cars[i].provenienza = i;
            cars[i].direzione   = EstraiDirezione(i); // guaranteed != i

            pid_t pid = fork();
            if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }

            if (pid == 0) {
                // --------- CHILD: car coming from street i ---------
                struct sigaction sa_ch = {0};
                sa_ch.sa_handler = on_term_auto;        // global handler
                sigaction(SIGTERM, &sa_ch, NULL);

                sem_t *child_sync = sem_open(SEM_PRINT_SYNC, 0);
                if (child_sync == SEM_FAILED) { perror("child sem_open " SEM_PRINT_SYNC); exit(1); }
                sem_t *log_sem = sem_open(SEM_LOG, 0);
                if (log_sem == SEM_FAILED) { perror("child sem_open " SEM_LOG); exit(1); }

                // per-car
                char s_go[64], s_ack[64];
                snprintf(s_go,  sizeof s_go,  "/Semaforo_%d",       i);
                snprintf(s_ack, sizeof s_ack, "/Semaforoscrivi_%d", i);

                sem_t *sem_go  = sem_open(s_go,  O_CREAT, 0600, 0);
                sem_t *sem_ack = sem_open(s_ack, O_CREAT, 0600, 0);
                if (sem_go==SEM_FAILED || sem_ack==SEM_FAILED) { perror("auto sem_open per-auto"); exit(1); }

                // creation announcement (synchronized with parent)
                log_linef(log_sem, "[auto %d] creata (dest %d)\n", i, cars[i].direzione);
                sem_post(child_sync);

                // wait for go signal from incrocio (if SIGTERM reaches child, exit cleanly)
                if (robust_sem_wait_stop(sem_go, &stop_auto) == -1) {
                    sem_close(child_sync); sem_close(log_sem);
                    sem_close(sem_go); sem_close(sem_ack);
                    exit(0);
                }

                // write to auto.txt serialized by mutex
                sem_t *file_mtx = sem_open(SEM_FILE, 0);
                if (file_mtx == SEM_FAILED) { perror("auto sem_open " SEM_FILE); exit(1); }
                if (robust_sem_wait_stop(file_mtx, &stop_auto) == -1) {
                    sem_close(child_sync); sem_close(log_sem);
                    sem_close(sem_go); sem_close(sem_ack);
                    sem_close(file_mtx);
                    exit(0);
                }
                FILE *fa = fopen("auto.txt", "a");
                if (fa) { fprintf(fa, "%d\n", i); fclose(fa); } else { perror("fopen auto.txt"); }
                sem_post(file_mtx);

                // log message and ACK to incrocio
                log_linef(log_sem, "[auto %d] attraversamento completato\n", i);
                sem_post(sem_ack);

                // child cleanup
                sem_close(child_sync);
                sem_close(log_sem);
                sem_close(sem_go);
                sem_close(sem_ack);
                sem_close(file_mtx);
                exit(0);
            }

            // --------- PARENT: record PID and print after child ---------
            cars[i].pidauto = pid;
            robust_sem_wait_stop(sem_print_sync, NULL);
            log_linef(sem_log, "[garage] auto %d (pid=%d) diretta a %d\n", i, (int)pid, cars[i].direzione);
        }

        // notify incrocio that batch is ready
        sem_post(sem_batch);

        // wait for all cars in current batch to terminate
        for (int i = 0; i < NUM_STRADE; ++i) {
            int st;
            while (wait(&st) == -1 && errno == EINTR) { /* retry if interrupted */ }
        }

        // --- REQUIRED DELAY: wait between batches ---
        if (*runflag == 1) sleep(1);
    }
    // ================= END BATCH LOOP =================

    // garage cleanup
    munmap(cars, cars_sz); close(fd_cars);
    munmap(runflag, sizeof(int)); close(fd_flag);

    sem_close(sem_batch);
    sem_close(sem_garage_done);
    sem_close(sem_print_sync);
    sem_close(sem_file_mutex);
    sem_close(sem_log);
    sem_close(sem_nextbatch);

    // signal to incrocio that garage has completed (incrocio closes last)
    if (sem_post(sem_garage_done) == -1) perror("sem_post " SEM_GARAGE_DONE);

    return 0;
}