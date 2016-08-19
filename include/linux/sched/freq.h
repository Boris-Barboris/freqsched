#ifndef _SCHED_FREQUENCY_H
#define _SCHED_FREQUENCY_H

#include <linux/sched/prio.h>

static inline int fq_prio(int prio)
{
	if (prio == MAX_RT_PRIO - 1)
		return 1;
	return 0;
}

#define FREQ_PULL_PERIOD 100000000

#endif /* _SCHED_FREQUENCY_H */
