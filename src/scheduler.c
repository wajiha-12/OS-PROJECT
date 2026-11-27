#define _GNU_SOURCE   //for warnings
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "common.h"
#include "jobs.h"

#define TIME_UNIT_MS 200   //one unit takes 0.2 secs...The scheduler works in abstract time units(t=0, 1, 2...) but the job threads have to "do work" for each unit

static const char *ALGO_KEYS[5]  = {"fcfs", "sjf", "srtf", "rr", "prio"};
static const char *ALGO_NAMES[5] = {"FCFS", "SJF", "SRTF", "RR", "PRIO"};

//shared state
static struct Job jobs[maxJobs];
static int numJobs = 0;

//stateMtx protects jobs[] fields changed while running, ready[], readyCount,now_t, arrivalStop and the simulation counters
//outMtx protects stdout lines written by threads and writes to the log pipe so lines never interleave
static pthread_mutex_t stateMtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t outMtx   = PTHREAD_MUTEX_INITIALIZER;

static sem_t runSem[maxJobs];   /* run[i]  : dispatcher -> job i, "run one unit", init 0 */
static sem_t doneSem;    /* done....job -> dispatcher, "unit finished",  init 0 */
static sem_t arriveGo;      /* dispatcher -> arrival thread, "boundary t",init 0 */
static sem_t arriveAck;    /* arrival thread -> dispatcher, "arrivals done",init 0 */

static int now_t = 0;
static int ready[maxJobs];      /* indices of arrived, unfinished, not-running jobs */
static int readyCount = 0;
static int arrivalStop = 0;

static int  logFd = -1;      /* write end of the pipe to the logger, -1 = none */
static long unitNs = (long)TIME_UNIT_MS * 1000000L;

// ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○
//helpers           
// ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○
static void fatal(const char *msg){
    fprintf(stderr, "perror: %s\n", msg);
    
    if(logFd >= 0)
        close(logFd);

    exit(1);
}

static void sem_wait_ok(sem_t *s){
    if (sem_wait(s)== -1)
        fatal("sem_wait failed");
}

static void sleep_unit(void){
    struct timespec ts = { unitNs/1000000000L, unitNs%1000000000L };
    nanosleep(&ts, NULL);
}

/* one line to the logger process through the pipe */
static void log_line(const char *text){
    int n = (int)strlen(text);

    pthread_mutex_lock(&outMtx);
    if (logFd >= 0 && write(logFd, text, n) != n) {
        close(logFd);              /* logger died: stop logging, keep scheduling */
        
        logFd = -1;
    }
    pthread_mutex_unlock(&outMtx);
}

static void log_event(int t, const char *what, int pidNum){
    char text[100];

    snprintf(text, sizeof(text), "t=%d %s P%d\n", t, what, pidNum);
    log_line(text);
}

static void log_run(const char *inputName, const char *algoName){
    char text[300];

    snprintf(text, sizeof(text), "=== RUN input=%.200s algorithm=%s ===\n", inputName, algoName);
    log_line(text);
}

static void log_run_rr(const char *inputName, int quantum){
    char text[300];

    snprintf(text, sizeof(text), "=== RUN input=%.200s algorithm=RR quantum=%d ===\n", inputName, quantum);
    log_line(text);
}

// ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○
// threads                       
// ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○
static void *arrival_thread(void *arg){
    (void)arg;
    for (;;) {
        sem_wait_ok(&arriveGo);

        pthread_mutex_lock(&stateMtx);
        if (arrivalStop) {
            pthread_mutex_unlock(&stateMtx);
            break;
        }
        int t = now_t;
        for (int i = 0; i < numJobs; i++) {      /* jobs sorted by pid: same-time arrivals in pid order */
            if (jobs[i].arrival == t) {
                ready[readyCount++] = i;
                log_event(t, "ARRIVED", jobs[i].pidNum);
                pthread_mutex_lock(&outMtx);
                printf("[t=%2d] P%d arrived\n", t, jobs[i].pidNum);
                fflush(stdout);
                pthread_mutex_unlock(&outMtx);
            }
        }
        pthread_mutex_unlock(&stateMtx);

        sem_post(&arriveAck);
    }
    return NULL;
}

static void *job_thread(void *arg){
    int i = (int)(intptr_t)arg;

    for (;;) {
        sem_wait_ok(&runSem[i]);          /* blocks until the dispatcher lets us run */

        pthread_mutex_lock(&stateMtx);
        int t = now_t;
        int rem = jobs[i].remaining;
        pthread_mutex_unlock(&stateMtx);

        pthread_mutex_lock(&outMtx);
        printf("[t=%2d] P%d running (remaining %d)\n", t, jobs[i].pidNum, rem);
        fflush(stdout);
        pthread_mutex_unlock(&outMtx);
        sleep_unit();                        /* simulate one unit of CPU work */

        pthread_mutex_lock(&stateMtx);
        jobs[i].remaining--;
        int left = jobs[i].remaining;
        pthread_mutex_unlock(&stateMtx);

        sem_post(&doneSem);                  /* tell the dispatcher this unit is over */
        if (left == 0)
            break;
    }
    return NULL;
}

// ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○
// ready queue selection (caller holds stateMtx) 
// ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○
static int take_ready(int pos)
{
    int j = ready[pos];
    memmove(&ready[pos], &ready[pos + 1], (size_t)(readyCount - pos - 1) * sizeof(int));
    readyCount--;
    return j;
}

// is job a strictly better choice than job b under this algorithm?
// ties: earlier arrival, then smaller index= smaller job id.. array is sorted
static int better(int algo, int a, int b)
{
    int ka = 0, kb = 0;
    switch (algo) {
        case fcfs_algo: {
            ka = 0;                 
            kb = 0;                 
            break;
        }

        case sjf_algo:{  
            ka = jobs[a].burst;     
            kb = jobs[b].burst;     
            break;
        }
        
        
        case srtf_algo:{ 
            ka = jobs[a].remaining; 
            kb = jobs[b].remaining; 
            break;
        }
        
        case prio_algo: {
            ka = jobs[a].priority;  
            kb = jobs[b].priority;  
            break;
        }

        default: 
            break;
    }

    if (ka != kb)
        return ka < kb;
   
    if (jobs[a].arrival != jobs[b].arrival)
        return jobs[a].arrival < jobs[b].arrival;
    return a < b;
}

static int pick_ready(int algo)
{
    int best = -1;
    for (int p = 0; p < readyCount; p++)
        if (best == -1 || better(algo, ready[p], ready[best]))
            best = p;
    return best;
}

/* preemption only when STRICTLY better on the main metric....if equal then running job continues */
static int preempts(int algo, int cand, int running)
{
    if (algo == srtf_algo)
        return (jobs[cand].remaining < jobs[running].remaining);
   
    if (algo == prio_algo)
        return (jobs[cand].priority < jobs[running].priority);

    return 0;
}

// ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○
// Gantt chart    
// ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○
struct GanttEntry { 
    int start, end, label; 
};      /* label = job index, -1 = IDLE */

struct Gantt { 
    struct GanttEntry *e; 
    int count,cap; 
};

static void gantt_add(struct Gantt *g, int t, int label){
    if (g->count > 0 && g->e[g->count-1].label== label && g->e[g->count-1].end == t){
        g->e[g->count- 1].end = t+1;
        return;
    }
    
    if (g->count== g->cap) {
        g->cap = (g->cap)? (g->cap*2) : 64;
        g->e = realloc(g->e, (size_t)g->cap * sizeof(struct GanttEntry));
        
        if (g->e == NULL)
            fatal("out of memory");
    }
    
    g->e[g->count].start = t;
    g->e[g->count].end = t+1;
    g->e[g->count].label=label;
    g->count++;
}

static void gantt_print(const struct Gantt *g)
{
    printf("Gantt:");
    for (int i = 0; i < g->count; i++) {
        if (g->e[i].label == -1)
            printf(" [%d-%d:IDLE]", g->e[i].start, g->e[i].end);
        
        else
            printf(" [%d-%d:P%d]", g->e[i].start, g->e[i].end, jobs[g->e[i].label].pidNum);
    }
    
    printf("\n");

    if (g->count <= 30) {
        for (int i = 0; i < g->count; i++) {
            char cell[16];
            if (g->e[i].label == -1)
                snprintf(cell, sizeof(cell), "IDLE");
            else
                snprintf(cell, sizeof(cell), "P%d", jobs[g->e[i].label].pidNum);
            
            printf("|%-7s", cell);
        }
       
        printf("|\n");
        
        for (int i = 0; i < g->count; i++)
            printf("%-8d", g->e[i].start);
        
        printf("%d\n", g->count ? g->e[g->count-1].end: 0);
    }
}

// ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ
// one complete simulation of one algorithm Parts 2, 3, 4
// ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ
static void simulate(int algo, int quantum, const char *inputName, struct AlgoResult *res)
{
    /* reset per-run state */
    for (int i = 0; i < numJobs; i++) {
        jobs[i].remaining = jobs[i].burst;
        jobs[i].firstStart = -1;
        jobs[i].completion = 0;
    }
    
    now_t = 0;
    readyCount = 0;
    arrivalStop = 0;

    if(sem_init(&doneSem, 0, 0) || sem_init(&arriveGo, 0, 0) || sem_init(&arriveAck, 0, 0))
        fatal("sem_init failed");

    for (int i = 0; i < numJobs; i++){
        if (sem_init(&runSem[i], 0, 0)){
            fatal("sem_init failed");}
    }

    pthread_t jobTid[maxJobs], arrTid;
    for (int i = 0; i < numJobs; i++){
        if(pthread_create(&jobTid[i], NULL, job_thread, (void *)(intptr_t)i) != 0)
            fatal("pthread_create failed");
    }

    if (pthread_create(&arrTid, NULL, arrival_thread, NULL) != 0){
        fatal("pthread_create failed");
    }


    if (algo == rr_algo){
        printf("\n=== ALGORITHM: RR (quantum=%d) ===\n", quantum);
    }
    
    else{
        printf("\n=== ALGORITHM: %s ===\n", ALGO_NAMES[algo]);
    }
    
    if (algo == rr_algo){
        log_run_rr(inputName, quantum);
    }
    
    else{
        log_run(inputName, ALGO_NAMES[algo]);
    }
    
    struct Gantt gantt = {NULL, 0, 0};
    int t = 0, finished = 0, running = -1, sliceUsed = 0;
    int lastJob = -1, lastEnd = -1;          /* last job that executed, and when that unit ended */
    int ctxSwitches = 0, busyTicks = 0;

    while (finished < numJobs) {
        /* arrivals for boundary t, performed by the arrival thread */
        pthread_mutex_lock(&stateMtx);
        now_t = t;
        pthread_mutex_unlock(&stateMtx);
        sem_post(&arriveGo);
        sem_wait_ok(&arriveAck);

        /* choose who runs during [t, t+1) */
        pthread_mutex_lock(&stateMtx);

        if (algo == rr_algo) {
            /* expired job goes behind the jobs that just arrived */
            if (running != -1 && sliceUsed >= quantum) {
                ready[readyCount++] = running;
                running = -1;
            }
            if (running == -1 && readyCount > 0) {
                running = take_ready(0);
                sliceUsed = 0;
            }
        }
        
        else if (algo == srtf_algo || algo == prio_algo) {
            int pos = pick_ready(algo);
            if (pos != -1 && (running == -1 || preempts(algo, ready[pos], running))) {
                int nj = take_ready(pos);
                if (running != -1)
                    ready[readyCount++] = running;
                running = nj;
            }
        } 
        
        else {                /* FCFS, SJF: only choose when the CPU is free */
            if (running == -1) {
                int pos = pick_ready(algo);
                if (pos != -1)
                    running = take_ready(pos);
            }
        }

        if (running == -1) {       /* nothing ready: CPU idle for one unit */
            pthread_mutex_unlock(&stateMtx);
            pthread_mutex_lock(&outMtx);
            printf("[t=%2d] IDLE\n", t);
            fflush(stdout);
            pthread_mutex_unlock(&outMtx);
            sleep_unit();
            pthread_mutex_lock(&stateMtx);
            gantt_add(&gantt, t, -1);
            pthread_mutex_unlock(&stateMtx);
            t++;
            
            continue;
        }

        int chosen = running;
        int adjacent = (lastEnd == t);

        if (!(adjacent && lastJob == chosen)) {  //re-dispatch
            if (adjacent && lastJob != -1) {     /* if the curr job is a  different job, it is counted as a switch */
                ctxSwitches++;
                if (jobs[lastJob].remaining > 0)
                    log_event(t, "PREEMPTED", jobs[lastJob].pidNum);
                pthread_mutex_lock(&outMtx);
                printf("[t=%2d] ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ- context switch: P%d -> P%d ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ-\n", t, jobs[lastJob].pidNum, jobs[chosen].pidNum);
                fflush(stdout);
                pthread_mutex_unlock(&outMtx);
            }
            log_event(t, jobs[chosen].firstStart == -1 ? "STARTED" : "RESUMED", jobs[chosen].pidNum);
        }
        if (jobs[chosen].firstStart == -1)
            jobs[chosen].firstStart = t;
        pthread_mutex_unlock(&stateMtx);

        /*and the CPU to the job thread for one unit */
        sem_post(&runSem[chosen]);
        sem_wait_ok(&doneSem);

        /*bookkeeping */
        pthread_mutex_lock(&stateMtx);
        sliceUsed++;
        busyTicks++;
        gantt_add(&gantt, t, chosen);
        lastJob = chosen;
        lastEnd = t + 1;
        t++;
        
        if (jobs[chosen].remaining == 0) {
            jobs[chosen].completion = t;
            finished++;
            running = -1;
            log_event(t, "FINISHED", jobs[chosen].pidNum);
            pthread_mutex_lock(&outMtx);
            printf("[t=%2d] P%d finished\n", t, jobs[chosen].pidNum);
            fflush(stdout);
            pthread_mutex_unlock(&outMtx);
        }
        
        pthread_mutex_unlock(&stateMtx);
    }

    /* shut the helper threads down and join everything */
    pthread_mutex_lock(&stateMtx);
    arrivalStop = 1;
    pthread_mutex_unlock(&stateMtx);
    sem_post(&arriveGo);
    pthread_join(arrTid, NULL);
    for (int i = 0; i < numJobs; i++){       /* every job thread exits when its job reaches 0 */
        pthread_join(jobTid[i], NULL);
    }
    
    for (int i = 0; i < numJobs; i++){
        sem_destroy(&runSem[i]);
    }

    sem_destroy(&doneSem);
    sem_destroy(&arriveGo);
    sem_destroy(&arriveAck);

    /* ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ- metrics ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ- */
    memset(res, 0, sizeof(*res));
    snprintf(res->name, sizeof(res->name), "%s", ALGO_NAMES[algo]);
    res->quantum = (algo == rr_algo) ? quantum : 0;
    res->numJobs = numJobs;
    res->makespan = t;
    res->busyTicks = busyTicks;
    res->contextSwitches = ctxSwitches;

    for (int i = 0; i < numJobs; i++) {
        struct JobMetrics *m = &res->jobs[i];
        m->pidNum = jobs[i].pidNum;
        m->arrival = jobs[i].arrival;
        m->burst = jobs[i].burst;
        m->priority = jobs[i].priority;
        m->completion = jobs[i].completion;
        m->turnaround = jobs[i].completion - jobs[i].arrival;
        m->waiting = m->turnaround - jobs[i].burst;
        m->response = jobs[i].firstStart - jobs[i].arrival;
        res->totalTAT += m->turnaround;
        res->totalWT += m->waiting;
        res->totalRT += m->response;
    }
   
    res->avgWT = (double)res->totalWT / numJobs;
    res->avgTAT = (double)res->totalTAT / numJobs;
    res->avgRT = (double)res->totalRT / numJobs;
    res->util = 100.0 * busyTicks / t;

    /* ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ- Part 4 output ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ- */
    printf("\n");
    gantt_print(&gantt);
    printf("\n%-5s %5s %5s %5s %5s %5s %5s %5s\n", "PID", "AT", "BT", "PR", "CT", "TAT", "WT", "RT");
    for (int i = 0; i < numJobs; i++) {
        const struct JobMetrics *m = &res->jobs[i];
        
        printf("P%-4d %5d %5d %5d %5d %5d %5d %5d\n", m->pidNum, m->arrival, m->burst,
               m->priority, m->completion, m->turnaround, m->waiting, m->response);
    }
    
    printf("Average: WT=%.2f  TAT=%.2f  RT=%.2f\n", res->avgWT, res->avgTAT, res->avgRT);
    printf("CPU Utilization: %.2f%%\n", res->util);
    printf("Context Switches: %d\n", res->contextSwitches);
    fflush(stdout);

    free(gantt.e);
}

// ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ 
// ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ 
static void print_comparison(const struct AlgoResult *r, int n, int quantum)
{
    printf("\n=== COMPARISON (quantum=%d) ===\n", quantum);
    printf("%-20s %12s %15s %13s %17s %10s\n", "Algorithm", "Avg waiting", "Avg turnaround",
           "Avg response", "Context switches", "CPU util");
    for (int a = 0; a < n; a++) {
        char label[48];
       
        if (r[a].quantum > 0){
            snprintf(label, sizeof(label), "Round Robin (q=%d)", r[a].quantum);
        }        

        else{
            snprintf(label, sizeof(label), "%.31s", r[a].name);
        }
        printf("%-20s %12.2f %15.2f %13.2f %17d %9.2f%%\n", label, r[a].avgWT, r[a].avgTAT,r[a].avgRT, r[a].contextSwitches, r[a].util);
    }

    /* best algorithm per metric, compared on the exact integer totals */
    const char *labels[3] = {"Best AvgWT", "Best AvgTAT", "Best AvgRT"};
    for (int m = 0; m < 3; m++) {
        long best = LONG_MAX;
        
        for (int a = 0; a < n; a++) {
            long v = (m == 0) ? r[a].totalWT : (m == 1) ? r[a].totalTAT : r[a].totalRT;
            if (v < best)
                best = v;
        }
        printf("%s: ", labels[m]);
        int printed = 0;
        
        for (int a = 0; a < n; a++) {
            long v = (m == 0) ? r[a].totalWT : (m == 1) ? r[a].totalTAT : r[a].totalRT;
            if (v == best) {
                printf("%s%s", printed ? ", " : "", r[a].name);
                printed++;
            }
        }
        printf("\n");
    }
    fflush(stdout);
}

// ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ
// main 
// ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ
static struct AlgoResult results[maxAlgos];

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s <input_file> <fcfs|sjf|srtf|rr|prio|all> [quantum]\n", prog);
    fprintf(stderr, "       quantum (1..10000) is required for rr and all\n");
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);                 /* a dead logger must not kill the scheduler */

    if (argc < 3 || argc > 4) {
        usage(argv[0]);
        return 1;
    }

    /* ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ- command line ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ- */
    int firstAlgo = -1, lastAlgo = -1;
    
    if (strcmp(argv[2], "all") == 0) {
        firstAlgo = fcfs_algo;
        lastAlgo = rr_algo;                   /* the four core algorithms, as in the brief */
    }
    
    else {
        for (int i = 0; i < 5; i++)
            if (strcmp(argv[2], ALGO_KEYS[i]) == 0)
                firstAlgo = lastAlgo = i;
    }
    
    if (firstAlgo == -1) {
        fprintf(stderr, "Error: unknown algorithm '%s'\n", argv[2]);
        usage(argv[0]);
        return 1;
    }

    int quantum = 0;
    int needsQuantum = (firstAlgo <= rr_algo && lastAlgo >= rr_algo);
    
    
    if (needsQuantum && argc != 4) {
        fprintf(stderr, "Error: a quantum is required for rr and all\n");
        usage(argv[0]);
        return 1;
    }
    
    if (argc == 4) {
        int len = (int)strlen(argv[3]);
        int valid = (len >= 1 && len <= 9);
        for (int i = 0; i < len; i++)
            if (argv[3][i] < '0' || argv[3][i] > '9')
                valid = 0;
        
        if (valid)
            quantum = atoi(argv[3]);
        
        if (!valid || quantum < 1 || quantum > 10000) {
            fprintf(stderr, "Error: quantum must be a whole number between 1 and 10000\n");
            return 1;
        }
    }

    const char *envUnit = getenv("SCHED_UNIT_MS");
    if (envUnit != NULL && atoi(envUnit) > 0)
        unitNs = (long)atoi(envUnit) * 1000000L;

    /* ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ- Part 1: job table ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ- */
    if (load_jobs(argv[1], jobs, &numJobs) != 0)
        return 1;

    /* directory that holds this executable (logger and report live next to it) */
    char exePath[PATH_MAX], dir[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
   
    if (len <= 0)
        snprintf(dir, sizeof(dir), ".");
  
    else {
        exePath[len] = '\0';
        char *slash = strrchr(exePath, '/');
        if (slash != NULL)
            *slash = '\0';
        snprintf(dir, sizeof(dir), "%s", exePath);
    }

    char loggerPath[PATH_MAX + 16], reportPath[PATH_MAX + 16];
    snprintf(loggerPath, sizeof(loggerPath), "%s/logger", dir);
    snprintf(reportPath, sizeof(reportPath), "%s/report", dir);

    /* ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ- Part 5: start the logger process ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ- */
    int logPipe[2];
    if (pipe(logPipe) == -1) {
        fatal("pipe");
        return 1;
    }
    fflush(stdout);
    pid_t loggerPid = fork();
   
    if (loggerPid == -1) {
        fatal("fork (logger)");
        return 1;
    }
   
    if (loggerPid == 0) {
        // logger: read end of the pipe becomes stdin 
        close(logPipe[1]);       /* unused write end */
        if (dup2(logPipe[0], STDIN_FILENO) == -1)
            _exit(126);
        close(logPipe[0]);     /* original fd no longer needed */
        execl(loggerPath, "logger", "schedule.log", (char *)NULL);
        fatal("exec logger");
        _exit(127);
    }
    close(logPipe[0]); /* scheduler never reads the pipe */
    logFd = logPipe[1];

    /* ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ- Parts 2-4: run the algorithm(s) ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ- */
    int numAlgos = 0;
    for (int a = firstAlgo; a <= lastAlgo; a++)
        simulate(a, quantum, argv[1], &results[numAlgos++]);

    if (numAlgos > 1)
        print_comparison(results, numAlgos, quantum);

    // ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ finish the logger: close write end then wait to prevent zombiesₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ- 
    close(logFd);
    logFd = -1;
    int status = 0;
    waitpid(loggerPid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fprintf(stderr, "Warning: logger did not exit cleanly (status %d)\n", status);

    // ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ- Part 6: shared memory+ report process ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ- 
    int shmId = shmget(IPC_PRIVATE, sizeof(struct SharedReport), IPC_CREAT | 0600);
    if (shmId == -1) {
        fatal("shmget");
        return 1;
    }
    struct SharedReport *shm = shmat(shmId, NULL, 0);
    
    if (shm == (void *)-1) {
        fatal("shmat");
        shmctl(shmId, IPC_RMID, NULL);
        return 1;
    }
    memset(shm, 0, sizeof(*shm));
    
    if (sem_init(&shm->ready, 1, 0) == -1) { /* pshared=1, initial value 0 */
        fatal("sem_init");
        shmdt(shm);
        shmctl(shmId, IPC_RMID, NULL);
        return 1;
    }

    char shmArg[32];
    snprintf(shmArg, sizeof(shmArg), "%d", shmId);   /* the report attaches using this id */

    fflush(stdout);
    pid_t reportPid = fork();
    
    if (reportPid == -1) {
        fatal("fork (report)");
        sem_destroy(&shm->ready);
        shmdt(shm);
        shmctl(shmId, IPC_RMID, NULL);
        return 1;
    }
    if (reportPid == 0) {
        execl(reportPath, "report", shmArg, (char *)NULL);
        fatal("exec report");
        _exit(127);
    }

    //the report is now blocked on shm->ready... write the data then release it
    shm->numAlgos = numAlgos;
    memcpy(shm->results, results, (size_t)numAlgos * sizeof(struct AlgoResult));
    sem_post(&shm->ready);

    waitpid(reportPid, &status, 0);

    int rc = 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Warning: report did not exit cleanly (status %d)\n", status);
        rc = 1;
    }

    /* clean up the shared memory and its semaphore */
    sem_destroy(&shm->ready);
    shmdt(shm);
    shmctl(shmId, IPC_RMID, NULL);     /* removes the segment */
    return rc;
}