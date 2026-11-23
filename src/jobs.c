#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jobs.h"

//true if s is 1..9 decimal digits only.... negatives, '+', letters fail
static int all_digits(const char *s)
{
    int len = (int)strlen(s);
    
    if (len < 1 || len > 9)
        return 0;
    
    for (int i = 0; i < len; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    
    return 1;
}

int load_jobs(const char *path, struct Job jobs[], int *numJobsOut){
    FILE *f = fopen(path, "r");
    
    if(f== NULL) {
        perror("Error opening job file");
        
        return -1;
    }

    int numJobs = 0, lineNumber = 0;
    char line[1024];

    while (fgets(line, sizeof(line), f) != NULL) {
        lineNumber++;

        if (strlen(line) == sizeof(line) - 1 && line[sizeof(line) - 2] != '\n') {
            fprintf(stderr, "Error!!1 line %d is too long\n", lineNumber);
            fclose(f);
           
            return -1;
        }

        //skip blank and comment lines
        char *first = line;
        while (*first == ' ' || *first == '\t' || *first == '\r' || *first == '\n')
            first++;
        
        if (*first == '\0' || *first == '#')
            continue;

        char *fields[6];
        int numFields = 0;
        char *token= strtok(line," \t\r\n");
       
        while((token != NULL) && (numFields < 6)) {
            fields[numFields++] = token;
            token = strtok(NULL, " \t\r\n");
       
        }

        if((numFields != 3) && (numFields != 4)) {
            fprintf(stderr, "Error!! must have 3 fields\n", lineNumber);
            fclose(f);
       
            return -1;
        }

        /* id: 'P' followed by a number without a leading zero */
        int pidLen = (int)strlen(fields[0]);
        int pidValid = (fields[0][0] == 'P' && pidLen >= 2 && pidLen <= 10 && fields[0][1] != '0');
        for (int i = 1; i < pidLen; i++)
            if (fields[0][i] < '0' || fields[0][i] > '9')
                pidValid = 0;
        if (!pidValid) {
            fprintf(stderr, "Error: line %d has an invalid job id '%s'\n", lineNumber, fields[0]);
            fclose(f);
            return -1;
        }

        /* arrival 0..10000, burst 1..10000, priority 0..1000000 (default 0) */
        int low[3]  = {0, 1, 0};
        int high[3] = {10000, 10000, 1000000};
        int values[3] = {0, 0, 0};
        int numValues = (numFields == 4) ? 3 : 2;

        for (int k = 0; k < numValues; k++) {
            if (!all_digits(fields[k + 1])) {
                fprintf(stderr, "Error!! invalid number in line %d\n", lineNumber);
                fclose(f);
                
                return -1;
            }

            values[k] = atoi(fields[k + 1]);
           
            if (values[k] < low[k] || values[k] > high[k]) {
                fprintf(stderr, "Error: line %d has a number out of range '%s'\n",
                        lineNumber, fields[k + 1]);
                fclose(f);
               
                return -1;
            }
        }

        if (numJobs == maxJobs) {
            fprintf(stderr, "Error: more than %d jobs\n", maxJobs);
            fclose(f);
           
            return -1;
        }

        int pidNum = atoi(fields[0] + 1);
        for (int i = 0; i < numJobs; i++) {
            if (jobs[i].pidNum == pidNum) {
                fprintf(stderr, "Error: line %d has a duplicate job id P%d\n", lineNumber, pidNum);
                fclose(f);
             
                return -1;
            }
        }

        jobs[numJobs].pidNum = pidNum;
        jobs[numJobs].arrival=values[0];
        jobs[numJobs].burst= values[1];
        jobs[numJobs].priority =values[2];
        jobs[numJobs].remaining= values[1];
        jobs[numJobs].firstStart= -1;
        jobs[numJobs].completion=0;
        
        numJobs++;
    }

    if (ferror(f)) {
        perror("Error reading job file");
        fclose(f);
        
        return -1;
    }

    fclose(f);

    if (numJobs == 0) {
        fprintf(stderr, "Error: job file is empty (no jobs found)\n");
    
        return -1;
    }

    //same time arrivals and final ties follow the smaller ID
    for(int i=0; i<(numJobs-1); i++){
        for (int j=0; j<(numJobs-1- i); j++) {
            if (jobs[j].pidNum > jobs[j+1].pidNum) {
                struct Job tmp = jobs[j];
                jobs[j]= jobs[j+1];
                jobs[j+1] = tmp;
            }
        }
    }

    *numJobsOut = numJobs;
    return 0;
}
