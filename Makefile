CC = gcc
CFLAGS= -Wall -Wextra -pthread -O2 -std=gnu11

all: scheduler logger report

scheduler: src/scheduler.c src/jobs.c src/jobs.h src/common.h
	$(CC) $(CFLAGS) -o $@ src/scheduler.c src/jobs.c

logger: src/logger.c
	$(CC) $(CFLAGS) -o $@ src/logger.c

report: src/report.c src/common.h
	$(CC) $(CFLAGS) -o $@ src/report.c

#Regenerates results.txt(comparison table of "all" mode for every input, quantum 2).
#SCHED_UNIT_MS=1 makes one simulated time unit last 1 ms so this runs in seconds.
results: all
	@rm -f results.txt schedule.log
	@for f in inputs/jobs*.txt; do \
	    echo "##### $$f  (all algorithms, quantum 2) #####" >> results.txt; \
	    SCHED_UNIT_MS=1 ./scheduler $$f all 2 | sed -n '/^=== COMPARISON/,/^Best AvgRT/p' >> results.txt; \
	    echo >> results.txt; \
	done
	@echo "results.txt written"

clean:
	rm -f scheduler logger report schedule.log

.PHONY: all results clean