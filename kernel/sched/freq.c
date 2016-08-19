/*
 * Frequency Scheduling Class (SCHED_FREQUENCY)
 *
 * Класс планировки, предназначенный для периодичных задач,
 * минимизирующий среднеквадратичное отклонение от желаемой частоты.
 * Задачи, неспособные выполнится за требуемое время, 
 * будут выполняться через кратные периоду промежутки времени.
 *
 */

#include "sched.h"

struct dl_bandwidth def_fq_bandwidth;


static inline struct task_struct *fq_task_of(struct sched_fq_entity *fq_se)
{
	return container_of(fq_se, struct task_struct, fq);
}

static inline struct rq *rq_of_fq_rq(struct fq_rq *fq_rq)
{
	return container_of(fq_rq, struct rq, fq);
}

static inline struct fq_rq *fq_rq_of_se(struct sched_fq_entity *fq_se)
{
	struct task_struct *p = fq_task_of(fq_se);
	struct rq *rq = task_rq(p);

	return &rq->fq;
}

static inline int on_fq_rq(struct sched_fq_entity *fq_se)
{
	return !RB_EMPTY_NODE(&fq_se->rb_node);
}

static inline int is_leftmost(struct task_struct *p, struct fq_rq *fq_rq)
{
	struct sched_fq_entity *fq_se = &p->fq;
	return fq_rq->rb_leftmost == &fq_se->rb_node;
}

void init_fq_bandwidth(struct dl_bandwidth *fq_b, u64 period, u64 runtime)
{
	raw_spin_lock_init(&fq_b->dl_runtime_lock);
	fq_b->dl_period = period;
	fq_b->dl_runtime = runtime;
}


extern void init_dl_bw(struct dl_bw *dl_b);


void init_fq_rq(struct fq_rq *fq_rq, struct rq *rq)
{
	fq_rq->rb_root = RB_ROOT;
	
#ifdef CONFIG_SMP	
	fq_rq->earliest_wakeup.curr_fin = fq_rq->earliest_wakeup.next_wakeup = 0;
	fq_rq->fq_nr_running = 0;
	fq_rq->fq_nr_migratory = 0;
	fq_rq->overloaded = 0;
	fq_rq->pull_time = 0;
	fq_rq->pushable_fq_tasks_root = RB_ROOT;
#else	
	init_dl_bw(&fq_rq->fq_bw);
#endif
}


static DEFINE_PER_CPU(cpumask_var_t, local_cpu_mask_fq);


void init_sched_fq_class(void)
{
	unsigned int i;

	for_each_possible_cpu(i)
		zalloc_cpumask_var_node(&per_cpu(local_cpu_mask_fq, i),
		GFP_KERNEL, cpu_to_node(i));
}




#ifdef CONFIG_SMP

static inline int fq_overloaded(struct rq *rq)
{
	return atomic_read(&rq->rd->fqo_count);
}

static inline void fq_set_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	cpumask_set_cpu(rq->cpu, rq->rd->fqo_mask);
	smp_wmb();
	atomic_inc(&rq->rd->fqo_count);
}

static inline void fq_clear_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	atomic_dec(&rq->rd->fqo_count);
	cpumask_clear_cpu(rq->cpu, rq->rd->fqo_mask);
}


static void update_fq_migration(struct fq_rq *fq_rq)
{
	if (fq_rq->fq_nr_migratory && fq_rq->fq_nr_running > 1) {
		if (!fq_rq->overloaded) {
			fq_set_overload(rq_of_fq_rq(fq_rq));
			fq_rq->overloaded = 1;
		}
	}
	else if (fq_rq->overloaded) {
		fq_clear_overload(rq_of_fq_rq(fq_rq));
		fq_rq->overloaded = 0;
	}
}

static void inc_fq_migration(struct sched_fq_entity *fq_se, struct fq_rq *fq_rq)
{
	struct task_struct *p = fq_task_of(fq_se);

	if (p->nr_cpus_allowed > 1)
		fq_rq->fq_nr_migratory++;

	update_fq_migration(fq_rq);
}

static void dec_fq_migration(struct sched_fq_entity *fq_se, struct fq_rq *fq_rq)
{
	struct task_struct *p = fq_task_of(fq_se);

	if (p->nr_cpus_allowed > 1)
		fq_rq->fq_nr_migratory--;

	update_fq_migration(fq_rq);
}

static inline bool fq_time_before(u64 a, u64 b)
{
	return (s64)(a - b) < 0;
}

static void dequeue_pushable_fq_task(struct rq *rq, struct task_struct *p);

static void enqueue_pushable_fq_task(struct rq *rq, struct task_struct *p)
{
	struct fq_rq *fq_rq = &rq->fq;
	
	struct rb_node **link = &fq_rq->pushable_fq_tasks_root.rb_node;
	struct rb_node *parent = NULL;
	struct task_struct *entry;
	int leftmost = 1;

	if (unlikely(!RB_EMPTY_NODE(&p->pushable_fq_tasks)))
	{
		dequeue_pushable_fq_task(rq, p);
		link = &fq_rq->pushable_fq_tasks_root.rb_node;
	}		
	
	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct task_struct,
			pushable_fq_tasks);
		if (fq_time_before(p->fq.wakeup, entry->fq.wakeup))
			link = &parent->rb_left;
		else 
		{
			link = &parent->rb_right;
			leftmost = 0;
		}
	}

	if (leftmost)
		fq_rq->pushable_fq_tasks_leftmost = &p->pushable_fq_tasks;

	rb_link_node(&p->pushable_fq_tasks, parent, link);
	rb_insert_color(&p->pushable_fq_tasks, &fq_rq->pushable_fq_tasks_root);
}

static void dequeue_pushable_fq_task(struct rq *rq, struct task_struct *p)
{
	struct fq_rq *fq_rq = &rq->fq;
	
	if (RB_EMPTY_NODE(&p->pushable_fq_tasks))
		return;

	if (fq_rq->pushable_fq_tasks_leftmost == &p->pushable_fq_tasks) {
		struct rb_node *next_node = rb_next(&p->pushable_fq_tasks);
		fq_rq->pushable_fq_tasks_leftmost = next_node;
	}

	rb_erase(&p->pushable_fq_tasks, &fq_rq->pushable_fq_tasks_root);
	RB_CLEAR_NODE(&p->pushable_fq_tasks);
}


static inline int has_pushable_fq_tasks(struct rq *rq)
{
	return !RB_EMPTY_ROOT(&rq->fq.pushable_fq_tasks_root);
}

//static int push_fq_task(struct rq *rq);

static inline bool need_pull_fq_task(struct rq *rq, struct task_struct *prev)
{
	return fq_policy(prev->policy);
}

static inline void set_post_schedule(struct rq *rq)
{
	rq->post_schedule = has_pushable_fq_tasks(rq);
}

#else

static inline
void enqueue_pushable_fq_task(struct rq *rq, struct task_struct *p)
{
}

static inline
void dequeue_pushable_fq_task(struct rq *rq, struct task_struct *p)
{
}

static inline
void inc_fq_migration(struct sched_fq_entity *fq_se, struct fq_rq *fq_rq)
{
}

static inline
void dec_fq_migration(struct sched_fq_entity *fq_se, struct fq_rq *fq_rq)
{
}

static inline bool need_pull_fq_task(struct rq *rq, struct task_struct *prev)
{
	return false;
}

static inline int pull_fq_task(struct rq *rq)
{
	return 0;
}

static inline void set_post_schedule(struct rq *rq)
{
}
#endif /* CONFIG_SMP */


static inline void setup_new_fq_entity(struct sched_fq_entity *fq_se)
{
	struct fq_rq *fq_rq = fq_rq_of_se(fq_se);
	struct rq *rq = rq_of_fq_rq(fq_rq);
	
	fq_se->wakeup = rq_clock(rq) + fq_se->fq_period;
	fq_se->runtime = 0;
	fq_se->prev_runtime = 0;
	fq_se->fq_new = 0;
}


static void update_fq_entity(struct sched_fq_entity *fq_se)
{
	struct fq_rq *fq_rq = fq_rq_of_se(fq_se);
	struct rq *rq = rq_of_fq_rq(fq_rq);
	
	if (fq_se->fq_new) {
		setup_new_fq_entity(fq_se);
		return;
	}	
	
	if (fq_time_before(fq_se->wakeup, rq_clock(rq))) 
	{
		fq_se->wakeup = fq_se->wakeup + 
			(1 + (rq_clock(rq) - fq_se->wakeup) / fq_se->fq_period) * fq_se->fq_period;
		fq_se->runtime = 0;		
	}
}

extern bool sched_rt_bandwidth_account(struct rt_rq *rt_rq);

static void update_curr_fq(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct sched_fq_entity *fq_se = &curr->fq;
	u64 delta_exec;		

	if (curr->sched_class != &fq_sched_class || !on_fq_rq(fq_se))
		return;
	
	delta_exec = rq_clock(rq) - curr->se.exec_start;
	if (unlikely((s64)delta_exec <= 0))
		return;
	
	schedstat_set(curr->se.statistics.exec_max,
		max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);

	curr->se.exec_start = rq_clock(rq);
	cpuacct_charge(curr, delta_exec);

	sched_rt_avg_update(rq, delta_exec);
	
	fq_se->runtime += delta_exec;
	
	if (rt_bandwidth_enabled()) {
		struct rt_rq *rt_rq = &rq->rt;

		raw_spin_lock(&rt_rq->rt_runtime_lock);
		if (sched_rt_bandwidth_account(rt_rq))
			rt_rq->rt_time += delta_exec;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
	}
}

static struct task_struct *next_earliest_wakeup_task(struct fq_rq *fq_rq)
{
	struct rb_node *next_node = fq_rq->rb_leftmost;
	struct sched_fq_entity *fq_se;
	struct task_struct *p = NULL;
	
	next_node = rb_next(next_node);
	if (next_node && !RB_EMPTY_NODE(next_node)) {
		fq_se = rb_entry(next_node, struct sched_fq_entity, rb_node);
		p = fq_task_of(fq_se);
		return p;
	}
	return NULL;
}



static void update_fq_rq_next_wakeup(struct fq_rq *fq_rq)
{
	struct task_struct *tsk;
	if (fq_rq->fq_nr_running < 2)
		fq_rq->earliest_wakeup.next_wakeup = 0;
	else
	{
		tsk = next_earliest_wakeup_task(fq_rq);
		fq_rq->earliest_wakeup.next_wakeup = tsk->fq.wakeup;
	}
}


static int pick_fq_task_ok(struct rq *rq, struct task_struct *p, int cpu)
{
	if (!task_running(rq, p) &&
		(cpu < 0 || cpumask_test_cpu(cpu, &p->cpus_allowed)) &&
		(p->nr_cpus_allowed > 1))
		return 1;

	return 0;
}


static struct task_struct *pick_next_earliest_wakeup_task(struct rq *rq, int cpu)
{
	struct rb_node *next_node = rq->fq.rb_leftmost;
	struct sched_fq_entity *fq_se;
	struct task_struct *p = NULL;
	
	next_node = rb_next(next_node);
	if (next_node) {
		fq_se = rb_entry(next_node, struct sched_fq_entity, rb_node);
		p = fq_task_of(fq_se);

		if (pick_fq_task_ok(rq, p, cpu))
			return p;
	}

	return NULL;
}


static inline void inc_fq_tasks(struct sched_fq_entity *fq_se, struct fq_rq *fq_rq)
{
	fq_rq->fq_nr_running++;		
	add_nr_running(rq_of_fq_rq(fq_rq), 1);	

	update_fq_rq_next_wakeup(fq_rq);
}


static inline void dec_fq_tasks(struct sched_fq_entity *fq_se, struct fq_rq *fq_rq)
{
	fq_rq->fq_nr_running--;
	sub_nr_running(rq_of_fq_rq(fq_rq), 1);

	update_fq_rq_next_wakeup(fq_rq);
}

static void __enqueue_fq_entity(struct sched_fq_entity *fq_se)
{
	struct fq_rq *fq_rq = fq_rq_of_se(fq_se);	
	struct rb_node **link = &fq_rq->rb_root.rb_node;
	struct rb_node *parent = NULL;
	struct sched_fq_entity *entry;
	int leftmost = 1;

	BUG_ON(!RB_EMPTY_NODE(&fq_se->rb_node));

	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct sched_fq_entity, rb_node);
		
		if (fq_time_before(fq_se->wakeup, entry->wakeup))
			link = &parent->rb_left;
		else 
		{
			link = &parent->rb_right;
			leftmost = 0;
		}
	}	
	
	if (leftmost)
		fq_rq->rb_leftmost = &fq_se->rb_node;
	
	rb_link_node(&fq_se->rb_node, parent, link);	
	rb_insert_color(&fq_se->rb_node, &fq_rq->rb_root);
	
	inc_fq_tasks(fq_se, fq_rq);
}


static void enqueue_fq_entity(struct sched_fq_entity *fq_se, int flags)
{	
	update_fq_entity(fq_se);	
	__enqueue_fq_entity(fq_se);
}

static void enqueue_task_fq(struct rq *rq, struct task_struct *p, int flags)
{
	enqueue_fq_entity(&p->fq, flags);

	if (!task_current(rq, p) && p->nr_cpus_allowed > 1)
	{
		inc_fq_migration(&p->fq, &rq->fq);
		enqueue_pushable_fq_task(rq, p);
	}
}

static void dequeue_fq_entity(struct sched_fq_entity *fq_se)
{
	struct fq_rq *fq_rq = fq_rq_of_se(fq_se);

	if (RB_EMPTY_NODE(&fq_se->rb_node))
		return;

	if (fq_rq->rb_leftmost == &fq_se->rb_node) {
		struct rb_node *next_node;

		next_node = rb_next(&fq_se->rb_node);
		fq_rq->rb_leftmost = next_node;
	}

	rb_erase(&fq_se->rb_node, &fq_rq->rb_root);
	RB_CLEAR_NODE(&fq_se->rb_node);

	dec_fq_tasks(fq_se, fq_rq);
}


static void __dequeue_task_fq(struct rq *rq, struct task_struct *p, int flags)
{
	dequeue_fq_entity(&p->fq);
	if ((p->nr_cpus_allowed > 1) && !task_current(rq, p))
	{
		dec_fq_migration(&p->fq, &rq->fq);
		dequeue_pushable_fq_task(rq, p);
	}	
}

static void dequeue_task_fq(struct rq *rq, struct task_struct *p, int flags)
{
	update_curr_fq(rq);
	__dequeue_task_fq(rq, p, flags);
}


static void yield_task_fq(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	update_curr_fq(rq);

	if (p->fq.runtime > 0) 
	{
		p->fq.prev_runtime = p->fq.runtime;
		p->fq.fq_yielded = 1;
		p->fq.runtime = 0;
	}
		
	if (p->fq.fq_period > 0)
		p->fq.wakeup += ((rq_clock(rq) - p->fq.wakeup) / p->fq.fq_period + 1) *
			p->fq.fq_period;
	else
		p->fq.wakeup = rq_clock(rq) + (u64)10000000;
		
	__dequeue_task_fq(rq, p, 0);
	enqueue_task_fq(rq, p, ENQUEUE_REPLENISH);
	resched_task(p);
}


static void check_preempt_curr_fq(struct rq *rq, struct task_struct *p, int flags) 
{
	if (p->prio < rq->curr->prio) {
		resched_task(rq->curr);
		return;
	}
}


static int pull_fq_task(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu, ret = 0, cpu;
	struct task_struct *p;
	struct rq *src_rq;
	u64 dmin = LONG_MAX;

	if (likely(!fq_overloaded(this_rq)))
		return 0;

	smp_rmb();
	
	for_each_cpu(cpu, this_rq->rd->fqo_mask) 
	{
		if (this_cpu == cpu)
			continue;

		src_rq = cpu_rq(cpu);	
		
		if (!this_rq->fq.fq_nr_migratory ||
			fq_time_before(this_rq->fq.earliest_wakeup.next_wakeup,
			src_rq->fq.earliest_wakeup.next_wakeup))
			continue;
		
		double_lock_balance(this_rq, src_rq);
		
		if (src_rq->fq.fq_nr_running <= 1)
			goto skip;

		p = pick_next_earliest_wakeup_task(src_rq, this_cpu);
		
		if (p && fq_time_before(p->fq.wakeup + p->fq.prev_runtime, dmin) &&
			(!this_rq->fq.fq_nr_running ||
			fq_time_before(p->fq.wakeup + p->fq.prev_runtime,
			this_rq->fq.earliest_wakeup.next_wakeup))) 
		{
			WARN_ON(p == src_rq->curr);
			WARN_ON(!p->on_rq);

			ret = 1;
			
			deactivate_task(src_rq, p, 0);
			set_task_cpu(p, this_cpu);
			activate_task(this_rq, p, 0);
			dmin = p->fq.wakeup;
			
			goto skip;
		}
	skip:
		double_unlock_balance(this_rq, src_rq);
	}

	return ret;
}



static struct sched_fq_entity *pick_next_fq_entity(struct rq *rq, struct fq_rq *fq_rq)
{
	struct sched_fq_entity *ret;
	struct rb_node *left = fq_rq->rb_leftmost;
	if (!left)
		return NULL;
	ret = rb_entry(left, struct sched_fq_entity, rb_node);
	if (ret)
	{
		if (ret->wakeup > rq_clock(rq))
			return NULL;	
		else
			return ret;
	}
	return NULL;
}


struct task_struct *pick_next_task_fq(struct rq *rq, struct task_struct *prev)
{
	struct sched_fq_entity *fq_se;
	struct task_struct *p;
	struct fq_rq *fq_rq;

	fq_rq = &rq->fq;	
	
	if (rq_clock(rq) - fq_rq->pull_time > (u64)FREQ_PULL_PERIOD)
	{
		fq_rq->pull_time = rq_clock(rq);
		pull_fq_task(rq);
		if (rq->stop && rq->stop->on_rq)
			return RETRY_TASK;
	}
	
	if (prev->sched_class == &fq_sched_class)
		update_curr_fq(rq);
	
	if (!fq_rq->fq_nr_running)
		return NULL;
	
	fq_se = pick_next_fq_entity(rq, fq_rq);
	if (!fq_se)
		return NULL;
	
	BUG_ON(!fq_se);
	
	put_prev_task(rq, prev);

	p = fq_task_of(fq_se);
	p->se.exec_start = rq_clock(rq);
	
	dequeue_pushable_fq_task(rq, p);

	set_post_schedule(rq);

	return p;
}


static void put_prev_task_fq(struct rq *rq, struct task_struct *p)
{
	update_curr_fq(rq);
	if (on_fq_rq(&p->fq) && p->nr_cpus_allowed > 1)
		enqueue_pushable_fq_task(rq, p);
}


static int select_task_rq_fq(struct task_struct *p, int cpu, int sd_flag, int flags)
{
	struct task_struct *curr;
	struct rq *rq;

	if (sd_flag != SD_BALANCE_WAKE && sd_flag != SD_BALANCE_FORK)
		goto out;

	rq = cpu_rq(cpu);

	rcu_read_lock();
	curr = ACCESS_ONCE(rq->curr); /* unlocked access */

	
	if (likely((curr->sched_class == &fq_sched_class) && (p->nr_cpus_allowed > 1)))
	{
		int target = -1, i_cpu;
		unsigned long min_fq = 10000000;
		
		for_each_cpu(i_cpu, sched_domain_span(rq->sd))
		{			
			if (cpu_rq(i_cpu)->fq.fq_nr_running < min_fq)
			{
				min_fq = cpu_rq(i_cpu)->fq.fq_nr_running;
				target = i_cpu;
				if (min_fq < 1)
					break;
			}
		}
		if (target != -1)
			cpu = target;
	}
	rcu_read_unlock();

out:
	return cpu;
}


static void rq_online_fq(struct rq *rq)
{
	if (rq->fq.overloaded)
		fq_set_overload(rq);
}


static void rq_offline_fq(struct rq *rq)
{
	if (rq->fq.overloaded)
		fq_clear_overload(rq);
}

static void set_curr_task_fq(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	p->se.exec_start = rq_clock(rq);

	dequeue_pushable_fq_task(rq, p);
}

static void task_tick_fq(struct rq *rq, struct task_struct *p, int queued)
{
	update_curr_fq(rq);
}

/*static void task_dead_fq(struct task_struct *p) {}*/

static void switched_to_fq(struct rq *rq, struct task_struct *p) 
{ 
	if (p->on_rq && rq->curr != p)
		if (task_has_fq_policy(rq->curr))
			check_preempt_curr_fq(rq, p, 0);
}

static void prio_changed_fq(struct rq *rq, struct task_struct *p, int oldprio)
{
	if (!p->on_rq && rq->curr != p)
		switched_to_fq(rq, p);
}

static void switched_from_fq(struct rq *rq, struct task_struct *p)
{
#ifdef CONFIG_SMP
	if (!rq->fq.fq_nr_running)
		pull_fq_task(rq);
#endif
}

static void task_dead_fq(struct task_struct *p)
{ }

const struct sched_class fq_sched_class = {
	.next = &fair_sched_class,
	.enqueue_task = enqueue_task_fq,	
	.dequeue_task = dequeue_task_fq,	
	.yield_task = yield_task_fq,		

	.check_preempt_curr = check_preempt_curr_fq,	

	.pick_next_task = pick_next_task_fq,	
	.put_prev_task = put_prev_task_fq,		

#ifdef CONFIG_SMP
	.select_task_rq = select_task_rq_fq,	
	//.set_cpus_allowed = set_cpus_allowed_fq,	
	.rq_online = rq_online_fq,				
	.rq_offline = rq_offline_fq,			
	//.post_schedule = set_post_schedule,		
	//.task_woken = task_woken_fq,			
#endif

	.set_curr_task = set_curr_task_fq,		
	.task_tick = task_tick_fq,				
	//.task_fork = task_fork_fq,
	.task_dead = task_dead_fq,			
		
	.prio_changed = prio_changed_fq,		
	.switched_from = switched_from_fq,		
	.switched_to = switched_to_fq,			
};