#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <time.h>

#define SCHED_FREQUENCY 7
#define SCHED_FIFO 1

#define SCHED_RESET_ON_FORK     0x01

#define u32 int
#define s32 unsigned
#define u64 unsigned long long

struct sched_attr {
	u32 size;

	u32 sched_policy;
	u64 sched_flags;

	/* SCHED_NORMAL, SCHED_BATCH */
	s32 sched_nice;

	/* SCHED_FIFO, SCHED_RR */
	u32 sched_priority;

	/* SCHED_DEADLINE */
	u64 sched_runtime;
	u64 sched_deadline;
	u64 sched_period;
};

int main (int argc, char **argv)
{
	pid_t pid;
	struct sched_attr params =
	{
		//.size = sizeof(struct sched_attr),
		.sched_policy = SCHED_FREQUENCY,
		//.sched_priority = 0,
		.sched_period = 200000000,
		//.sched_nice = 0,
	};
	int j = 50;
	int pcount = 3;
	int i = 0;
	pid_t f = 0;
	struct timespec ts;
	long long curnsec, prevnsec;
	
	clock_gettime(CLOCK_MONOTONIC, &ts);
	prevnsec = (ts.tv_sec * 1000000000) + ts.tv_nsec;
		
	for (; i < pcount; i++)
	{
		f = fork();
		if (!f)
			break;
	}
	if (f)
	{
		while (pcount) 
		{
			wait();
			pcount--;
		}
		puts("Finish");
		return 0;
	}
	else
	{
		pid = getpid();
		if (i == 0)
		{
			params.sched_period = 100000000;
		}
		int switch_code = syscall(__NR_sched_setattr, pid, &params, 0);
		if (switch_code)
		{
			// error
			puts("Error switching scheduler");
			return 1;
		}
		else
		{
			while (j)
			{
				clock_gettime(CLOCK_MONOTONIC, &ts);
				curnsec = (ts.tv_sec * 1000000000) + ts.tv_nsec;
				fprintf(stdout, "From frequency task %d, interval %lld\n", i, curnsec - prevnsec);
				j--;
				prevnsec = curnsec;
				if (j)
					sched_yield();
			}
			//fprintf(stdout, "Finished task %d\n", i);
		}
		return 0;
	}
}
