ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ
1. GROUP MEMBERS AND WORK SPLIT
ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ
Wajiha Abbasi, 24i-2059
Hanaa Sajid, 24i-2029
Aqsa Noor, 24i-2010

2. FILES
ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ  
src/common.h: structs shared by scheduler and report (shared-memory layout)
src/jobs.c: read + validate the job file
src/scheduler.c: main program (dispatcher, arrival thread, job threads)
src/logger.c : logger program (started by fork + dup2 + exec)
src/report.c: report program (started by fork + exec)
inputs/ : jobs1, jobs2,job3,jobs4.txt
results.txt: "all" mode comparison table for every input

3. HOW TO COMPILE AND RUN
ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ  
make: builds ./scheduler ./logger ./report  (-Wall -Wextra -pthread, no warnings)
make results: regenerates results.txt
make clean

./scheduler <input_file> <fcfs|sjf|srtf|rr|prio|all> [quantum]
./scheduler inputs/jobs1.txt fcfs
./scheduler inputs/jobs1.txt rr 2
./scheduler inputs/jobs1.txt all 2 (runs fcfs, sjf, srtf, rr and prints the comparison)
./scheduler inputs/jobs4.txt prio  (bonus: preemptive priority)

quantum (1..10000) is required for rr and all
One time unit = TIME_UNIT_MS (200 ms) defined at the top of src/scheduler.c
For quick testing: SCHED_UNIT_MS=1 ./scheduler inputs/jobs1.txt all 2
The logger writes schedule.log in the current directory (appended on every run)
./logger and ./report must stay in the same folder as ./scheduler

Input format: "P<id> <arrival> <burst> [priority]"
The optional 4th column is used only by the bonus 'prio' algorithm (default 0, lower= more important)
Limits: up to 50 jobs, arrival 0..10000, burst 1..10000, no leading zero in the id
Bad input(missing/empty file, negative or non-numeric numbers, duplicate ids, wrong field count) prints an error with the line number and exits with status 1

4. SYNCHRONISATION DESIGN
ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ
Threads (all in the scheduler process):
  * main thread = dispatcher. It owns the logical clock t.
  * arrival thread: enqueues jobs whose arrival time equals the clock.
  * one thread per job: runs exactly one time unit each time it is woken.

Mutexes
stateMtx protects the ready queue (ready[], readyCount), the clock now_t, the arrivalStop flag, and every job field that threads touch (remaining,firstStart, completion)....also protects the Gantt/metric counters
outMtx protects console output from threads and the writes to the logger pipe,so lines never interleave....lock order is always stateMtx and then outMtx

Semaphores (all unnamed, sem_init)       initial value
runSem[i]   dispatcher -> job thread i       0   job thread blocks here until chosen
doneSem     job thread -> dispatcher         0   "my time unit is finished"
arriveGo    dispatcher -> arrival thread     0   "clock is now t, do the arrivals"
arriveAck   arrival thread -> dispatcher     0   "arrivals for t are in the queue"
shm->ready  scheduler -> report process      0   process-shared, lives inside the shared memory segment


5. CONVENTIONS / ASSUMPTIONS
ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ
Ties: earlier arrival first, then smaller job id.
Round Robin: a job arriving at the moment another job's quantum expires enters the queue before the preempted job. If the expired job is alone in the queue it simply continues (no context switch)
SRTF checks at every time-unit boundary; on equal remaining time the running job continues
Priority (bonus) is preemptive with the same rule
Idle CPU shows as IDLE in the Gantt chart
first dispatch and switches to/from IDLE are not counted.
Turnaround = completion - arrival, waiting = turnaround - burst, response = first CPU time - arrival. CPU utilisation = busy units/total units from t=0
"all" runs the four core algorithms (FCFS, SJF, SRTF, RR) as in the brief..... prio is run on its own.
Each time unit is one dispatcher/job-thread handshake (the slice for RR/FCFS/SJF is tracked by the dispatcher) which lets arrivals happen in the middle of a long slice


ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ
6. BONUS
Priority scheduling (preemptive)


ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ
7. TESTING DONE
jobs1.txt RR q=2 acc to da brief's Gantt chart
Output of all algorithms on all inputs was compared against an independent
tick-by-tick simulator(Gantt, per-job table, context switches) are identical

ₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓₓ˚. ୭ ˚○◦˚.˚◦○˚ ୧ .˚ₓ
8. KNOWN LIMITATIONS
Linux only (uses /proc/self/exe, POSIX shm, POSIX semaphores)
The logger/report executables must be next to the scheduler executable
schedule.log is appended to, never truncated....I ran "make clean" to remove it
Large bursts with the default 200 ms unit take real time.... use SCHED_UNIT_MS to speed up
