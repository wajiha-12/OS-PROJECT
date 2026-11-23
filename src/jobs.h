#ifndef JOBS_H
#define JOBS_H

#include "common.h"

struct Job {
    int pidNum;
    int arrival;
    int burst;
    int priority; //deafult 0
    int remaining;
    int firstStart;    // -1 until the job first gets the CPU
    int completion;
};

//reads and validates the whole file. Jobs are returned sorted by pid no
//0= success... -1=  failure
int load_jobs(const char *path, struct Job jobs[], int *numJobs);

#endif
