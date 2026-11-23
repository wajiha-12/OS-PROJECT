//data shared between the scheduler and the report program....The scheduler fills a struvt SharedReport inside a POSIX shared memory segnent andd the report program maps the same segment and prints it
#ifndef COMMON_H
#define COMMON_H

#include <semaphore.h>

#define maxJobs  50
#define maxAlgos 5

enum { 
    fcfs_algo = 0, 
    sjf_algo = 1, 
    srtf_algo = 2, 
    rr_algo = 3,
    prio_algo = 4 };

struct JobMetrics {
    int pidNum;
    int arrival;
    int burst;
    int priority;
    int completion;
    int turnaround;
    int waiting;
    int response;
};

struct AlgoResult {
    char name[32];    //name of the algo
    int quantum;     //only for rr
    int numJobs;
    int makespan;     //total simulated time unitss
    int busyTicks;
    int contextSwitches;
    long totalWT,totalTAT, totalRT;   //for exact comaprison
    double avgWT, avgTAT,avgRT, util;
    struct JobMetrics jobs[maxJobs];
};

struct SharedReport {
    sem_t ready;              // unnamed.. initial value 0...Scheduler posts it after writing everything
    int numAlgos;
    struct AlgoResult results[maxAlgos];
    
};

#endif
