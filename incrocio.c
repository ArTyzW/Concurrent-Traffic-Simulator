// incrocio.c — coordinator (immediate exit on SIGTERM; no sleep/polling; ordered batches)
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
#include <semaphore.h>

#include "incrocio.h"   // NUM_STRADE, NESSUNA_AUTO, GetNextCar

// ---- interface/shared names with garage ----
typedef struct {
    pid_t pidauto;
    int   provenienza;
    int   direzione;
} CarInfo;

#define SHM_CARS        "/shared_auto"
#define SHM_FLAG        "/flag_termina"
#define SEM_READY_RES   "/semaforo_risorse"     // garage -> incrocio: resources created
#define SEM_BATCH       "/sem_gi"               // garage -> incrocio: batch ready
#define SEM_GARAGE_DONE "/semaforo_terminale"   // garage -> incrocio: proceed to shutdown
#define SEM_NEXTBATCH   "/sem_nextbatch"        // incrocio -> garage: start next batch
#define SEM_LOG         "/sem_log"              // serializes stdout printing

static volatile sig_atomic_t stop_requested = 0;

static void on_sigint(int s){ (void)s;
    const char m[]="[incrocio] SIGINT ignorato: termina con SIGTERM su incrocio.\n";
    (void)!write(STDERR_FILENO,m,sizeof m-1);
}
static void on_sigterm(int s){ (void)s;
    const char m[]="[incrocio] SIGTERM ricevuto: chiusura ordinata.\n";
    (void)!write(STDERR_FILENO,m,sizeof m-1);
    stop_requested = 1;   // do not exit immediately: clean shutdown after loop
}

/* FIX: pre-check stop_flag BEFORE entering sem_wait(). */
static int robust_sem_wait_stop(sem_t *s, volatile sig_atomic_t *stop_flag) {
    if (stop_flag && *stop_flag) { errno = EINTR; return -1; }  // <<< pre-check
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
    // --- signal handling ---
    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;  sigaction(SIGINT,  &sa, NULL);
    sa.sa_handler = on_sigterm; sigaction(SIGTERM, &sa, NULL);

    // --- create/open global semaphores (idempotent with O_CREAT) ---
    sem_t *sem_ready_res   = sem_open(SEM_READY_RES,   O_CREAT, 0600, 0);
    sem_t *sem_batch       = sem_open(SEM_BATCH,       O_CREAT, 0666, 0);
    sem_t *sem_garage_done = sem_open(SEM_GARAGE_DONE, O_CREAT, 0600, 0);
    sem_t *sem_nextbatch   = sem_open(SEM_NEXTBATCH,   O_CREAT, 0600, 0);
    sem_t *sem_log         = sem_open(SEM_LOG,         O_CREAT, 0600, 1);
    if (sem_ready_res==SEM_FAILED || sem_batch==SEM_FAILED || sem_garage_done==SEM_FAILED ||
        sem_nextbatch==SEM_FAILED || sem_log==SEM_FAILED) {
        perror("incrocio: sem_open global");
        exit(EXIT_FAILURE);
    }

    // --- wait for "resources ready" signal from garage ---
    if (robust_sem_wait_stop(sem_ready_res, &stop_requested) == -1) {
        sem_close(sem_ready_res);
        sem_close(sem_batch);
        sem_close(sem_garage_done);
        sem_close(sem_nextbatch);
        sem_close(sem_log);
        return 0;
    }
    sem_close(sem_ready_res);

    // --- open SHM segments created by garage ---
    int fd_cars = shm_open(SHM_CARS, O_RDWR, 0666);
    if (fd_cars == -1) { perror("shm_open " SHM_CARS); exit(EXIT_FAILURE); }
    size_t cars_sz = sizeof(CarInfo) * NUM_STRADE;
    CarInfo *cars = mmap(NULL, cars_sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd_cars, 0);
    if (cars == MAP_FAILED) { perror("mmap cars"); exit(EXIT_FAILURE); }

    int fd_flag = shm_open(SHM_FLAG, O_RDWR, 0666);
    if (fd_flag == -1) { perror("shm_open " SHM_FLAG); exit(EXIT_FAILURE); }
    int *runflag = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, fd_flag, 0);
    if (runflag == MAP_FAILED) { perror("mmap flag"); exit(EXIT_FAILURE); }

    // --- unblock the FIRST batch ---
    if (sem_post(sem_nextbatch) == -1) perror("sem_post " SEM_NEXTBATCH);

    // reset output file
    FILE *fp = fopen("incrocio.txt","w"); if (fp) fclose(fp);

    log_linef(sem_log, "[incrocio] pronto. In attesa dei batch...\n");

    // ====================== MAIN LOOP ======================
    while (!stop_requested && *runflag == 1) {
        // 1) wait for batch ready
        if (robust_sem_wait_stop(sem_batch, &stop_requested) == -1) {
            if (stop_requested) (void)sem_post(sem_nextbatch);  // wake up garage if waiting
            break;
        }
        if (stop_requested || *runflag != 1) {
            if (stop_requested) (void)sem_post(sem_nextbatch);
            break;
        }

        // 2) array setup for GetNextCar
        int dirs[NUM_STRADE];
        for (int i=0;i<NUM_STRADE;++i) dirs[i]=NESSUNA_AUTO;
        for (int i=0;i<NUM_STRADE;++i) {
            int from = cars[i].provenienza;
            int to   = cars[i].direzione;
            if (from>=0 && from<NUM_STRADE) dirs[from]=to;
        }

        int remaining=0;
        for (int i=0;i<NUM_STRADE;++i) if (dirs[i]!=NESSUNA_AUTO) remaining++;

        // 3) batch crossings
        while (remaining>0 && !stop_requested && *runflag==1) {
            int idx = GetNextCar(dirs);
            if (idx == NESSUNA_AUTO) break;

            char name_go[64], name_ack[64];
            snprintf(name_go,  sizeof name_go,  "/Semaforo_%d",       idx);
            snprintf(name_ack, sizeof name_ack, "/Semaforoscrivi_%d", idx);

            sem_t *sem_go  = sem_open(name_go,  0);
            sem_t *sem_ack = sem_open(name_ack, 0);
            if (sem_go==SEM_FAILED || sem_ack==SEM_FAILED) {
                log_linef(sem_log, "[incrocio] errore apertura semafori per-auto (idx=%d)\n", idx);
                if (sem_go  != SEM_FAILED) sem_close(sem_go);
                if (sem_ack != SEM_FAILED) sem_close(sem_ack);
                break;
            }

            // log to file
            int fd = open("incrocio.txt", O_CREAT|O_WRONLY|O_APPEND, 0644);
            if (fd>=0){ dprintf(fd, "%d\n", idx); close(fd); } else { perror("open incrocio.txt"); }

            // signal car to go and wait for acknowledgment (immediate exit if stop requested)
            if (stop_requested) { sem_close(sem_go); sem_close(sem_ack); break; }
            if (sem_post(sem_go) == -1) perror("sem_post GO");
            if (robust_sem_wait_stop(sem_ack, &stop_requested) == -1) {
                sem_close(sem_go); sem_close(sem_ack);
                break;
            }

            log_linef(sem_log, "[incrocio] passata auto da %d verso %d\n", idx, dirs[idx]);

            dirs[idx]=NESSUNA_AUTO;
            --remaining;

            sem_close(sem_go);
            sem_close(sem_ack);
        }

        // 4) batch completed
        if (sem_post(sem_nextbatch) == -1) perror("sem_post " SEM_NEXTBATCH);
        log_linef(sem_log, "----- batch completato -----\n");
    }
    // ==================== END MAIN LOOP ====================

    // request garage to stop and WAKE IT UP if it's waiting for a new batch
    *runflag = 0;
    (void)sem_post(sem_nextbatch);   // best-effort

    // DO NOT block on shutdown: try once and proceed (immediate exit)
    (void)sem_trywait(sem_garage_done);

    // cleanup
    munmap(cars, cars_sz); close(fd_cars);
    munmap(runflag, sizeof(int)); close(fd_flag);

    sem_close(sem_batch);
    sem_close(sem_garage_done);
    sem_close(sem_nextbatch);
    sem_close(sem_log);

    // unlink named IPC objects (descriptors in garage remain valid)
    shm_unlink(SHM_CARS);
    shm_unlink(SHM_FLAG);
    sem_unlink(SEM_READY_RES);
    sem_unlink(SEM_BATCH);
    sem_unlink(SEM_GARAGE_DONE);
    sem_unlink(SEM_NEXTBATCH);
    sem_unlink(SEM_LOG);

    return 0;
}