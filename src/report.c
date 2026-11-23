//Attaches to the shared memory segment, blocks on the semaphore stored in it until the scheduler has finished writing. then prints the final report
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/shm.h>
#include "common.h"

static void print_algo(const struct AlgoResult *r)
{
    if (r->quantum > 0){
        printf("ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ %s (quantum=%d) ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ\n", r->name, r->quantum);
    }
    
    else{
        printf("ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ %s ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ\n", r->name);
    }
    
    printf("%-5s %5s %5s %5s %5s %5s %5s %5s\n", "PID","AT","BT", "PR", "CT", "TAT", "WT", "RT");
    
    for (int i = 0; i < r->numJobs; i++) {
        const struct JobMetrics *m = &r->jobs[i];
        
        printf("P%-4d %5d %5d %5d %5d %5d %5d %5d\n", m->pidNum, m->arrival, m->burst,m->priority, m->completion, m->turnaround, m->waiting, m->response);
    }

    printf("Average waiting=%.2f  turnaround=%.2f  response=%.2f\n", r->avgWT, r->avgTAT, r->avgRT);
    printf("CPU utilization=%.2f%%  context switches=%d  makespan=%d\n\n",r->util, r->contextSwitches, r->makespan);
}

int main(int argc, char *argv[]){
    if (argc != 2) {
        fprintf(stderr, "usage: %s <shm_id>\n", argv[0]);
        return 2;
    }

    int shmId = atoi(argv[1]);
    struct SharedReport *shm = shmat(shmId,NULL, 0);
   
    if (shm == (void *)-1) {
        perror("report: shmat");
        return 1;
    }

    //wait until the scheduler says the data is complete
    if (sem_wait(&shm->ready) == -1) {
        perror("report: sem_wait");
        shmdt(shm);
        return 1;
    }

    printf("\nₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ FINAL REPORT (read from shared memory) ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ\n");
    
    for (int a = 0; a < shm->numAlgos; a++){
        print_algo(&shm->results[a]);
    }

    if (shm->numAlgos > 1) {
        printf("ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ Comparison ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ\n");
        
        printf("%-20s %12s %15s %13s %17s\n", "Algorithm", "Avg waiting", "Avg turnaround","Avg response", "Context switches");
        
        for (int a = 0; a < shm->numAlgos; a++) {
            const struct AlgoResult *r = &shm->results[a];
            char label[48];
            
            if (r->quantum > 0){
                snprintf(label, sizeof(label), "%s (q=%d)", r->name, r->quantum);
            }
            
            else{
                snprintf(label, sizeof(label), "%s", r->name);
            }
            
            printf("%-20s %12.2f %15.2f %13.2f %17d\n",label,r->avgWT, r->avgTAT,r->avgRT,r->contextSwitches);
        }
    }
    
    printf("ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ\n");

    shmdt(shm);


    return 0;
}