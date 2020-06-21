/* Copyright (c) 2014-2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)	"core_ctl: " fmt

#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/syscore_ops.h>

#include <trace/events/sched.h>
#include "sched.h"
#include "walt.h"

#define MAX_CPUS_PER_CLUSTER 6
#define MAX_CLUSTERS 2

struct cluster_data {
	bool inited;
	unsigned int min_cpus;
	unsigned int max_cpus;
	unsigned int offline_delay_ms;
	unsigned int busy_up_thres[MAX_CPUS_PER_CLUSTER];
	unsigned int busy_down_thres[MAX_CPUS_PER_CLUSTER];
	unsigned int active_cpus;
	unsigned int num_cpus;
	unsigned int nr_isolated_cpus;
	unsigned int nr_not_preferred_cpus;
	cpumask_t cpu_mask;
	unsigned int need_cpus;
	unsigned int task_thres;
	unsigned int max_nr;
	s64 need_ts;
	struct list_head lru;
	bool pending;
	spinlock_t pending_lock;
	bool enable;
	int nrrun;
	struct task_struct *core_ctl_thread;
	unsigned int first_cpu;
	unsigned int boost;
	struct kobject kobj;
};

struct cpu_data {
	bool is_busy;
	unsigned int busy;
	unsigned int cpu;
	bool not_preferred;
	struct cluster_data *cluster;
	struct list_head sib;
	bool isolated_by_us;
};

static DEFINE_PER_CPU(struct cpu_data, cpu_state);
static struct cluster_data cluster_state[MAX_CLUSTERS];
static unsigned int num_clusters;

#define for_each_cluster(cluster, idx) \
	for ((cluster) = &cluster_state[idx]; (idx) < num_clusters;\
		(idx)++, (cluster) = &cluster_state[idx])

static DEFINE_SPINLOCK(state_lock);
static void apply_need(struct cluster_data *state);
static void wake_up_core_ctl_thread(struct cluster_data *state);
static bool initialized;

static unsigned int get_active_cpu_count(const struct cluster_data *cluster);

/* ========================= sysfs interface =========================== */

struct core_ctl_attr {
	struct attribute attr;
	ssize_t (*show)(const struct cluster_data *, char *);
	ssize_t (*store)(struct cluster_data *, const char *, size_t count);
};

#define core_ctl_attr_ro(_name)		\
static struct core_ctl_attr _name =	\
__ATTR(_name, 0444, show_##_name, NULL)

#define core_ctl_attr_rw(_name)			\
static struct core_ctl_attr _name =		\
__ATTR(_name, 0644, show_##_name, store_##_name)

#define to_cluster_data(k) container_of(k, struct cluster_data, kobj)
#define to_attr(a) container_of(a, struct core_ctl_attr, attr)
static ssize_t show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct cluster_data *data = to_cluster_data(kobj);
	struct core_ctl_attr *cattr = to_attr(attr);
	ssize_t ret = -EIO;

	if (cattr->show)
		ret = cattr->show(data, buf);

	return ret;
}

static ssize_t store(struct kobject *kobj, struct attribute *attr,
		     const char *buf, size_t count)
{
	struct cluster_data *data = to_cluster_data(kobj);
	struct core_ctl_attr *cattr = to_attr(attr);
	ssize_t ret = -EIO;

	if (cattr->store)
		ret = cattr->store(data, buf, count);

	return ret;
}

static const struct sysfs_ops sysfs_ops = {
	.show	= show,
	.store	= store,
};

/* ==================== runqueue based core count =================== */

static struct sched_avg_stats nr_stats[NR_CPUS];

/*
 * nr_need:
 *   Number of tasks running on this cluster plus
 *   tasks running on higher capacity clusters.
 *   To find out CPUs needed from this cluster.
 *
 * For example:
 *   On dual cluster system with 4 min capacity
 *   CPUs and 4 max capacity CPUs, if there are
 *   4 small tasks running on min capacity CPUs
 *   and 2 big tasks running on 2 max capacity
 *   CPUs, nr_need has to be 6 for min capacity
 *   cluster and 2 for max capacity cluster.
 *   This is because, min capacity cluster has to
 *   account for tasks running on max capacity
 *   cluster, so that, the min capacity cluster
 *   can be ready to accommodate tasks running on max
 *   capacity CPUs if the demand of tasks goes down.
 */
static int compute_cluster_nr_need(int index)
{
	int cpu;
	struct cluster_data *cluster;
	int nr_need = 0;

	for_each_cluster(cluster, index) {
		for_each_cpu(cpu, &cluster->cpu_mask)
			nr_need += nr_stats[cpu].nr;
	}

	return nr_need;
}

/*
 * prev_misfit_need:
 *   Tasks running on smaller capacity cluster which
 *   needs to be migrated to higher capacity cluster.
 *   To find out how many tasks need higher capacity CPUs.
 *
 * For example:
 *   On dual cluster system with 4 min capacity
 *   CPUs and 4 max capacity CPUs, if there are
 *   2 small tasks and 2 big tasks running on
 *   min capacity CPUs and no tasks running on
 *   max cpacity, prev_misfit_need of min capacity
 *   cluster will be 0 and prev_misfit_need of
 *   max capacity cluster will be 2.
 */
static int compute_prev_cluster_misfit_need(int index)
{
	int cpu;
	struct cluster_data *prev_cluster;
	int prev_misfit_need = 0;

	/*
	 * Lowest capacity cluster does not have to
	 * accommodate any misfit tasks.
	 */
	if (index == 0)
		return 0;

	prev_cluster = &cluster_state[index - 1];

	for_each_cpu(cpu, &prev_cluster->cpu_mask)
		prev_misfit_need += nr_stats[cpu].nr_misfit;

	return prev_misfit_need;
}

static int compute_cluster_max_nr(int index)
{
	int cpu;
	struct cluster_data *cluster = &cluster_state[index];
	int max_nr = 0;

	for_each_cpu(cpu, &cluster->cpu_mask)
		max_nr = max(max_nr, nr_stats[cpu].nr_max);

	return max_nr;
}

static int cluster_real_big_tasks(int index)
{
	int nr_big = 0;
	int cpu;
	struct cluster_data *cluster = &cluster_state[index];

	if (index == 0) {
		for_each_cpu(cpu, &cluster->cpu_mask)
			nr_big += nr_stats[cpu].nr_misfit;
	} else {
		for_each_cpu(cpu, &cluster->cpu_mask)
			nr_big += nr_stats[cpu].nr;
	}

	return nr_big;
}

static void update_running_avg(void)
{
	struct cluster_data *cluster;
	unsigned int index = 0;
	unsigned long flags;
	int big_avg = 0;

	sched_get_nr_running_avg(nr_stats);

	spin_lock_irqsave(&state_lock, flags);
	for_each_cluster(cluster, index) {
		int nr_need, prev_misfit_need;

		if (!cluster->inited)
			continue;

		nr_need = compute_cluster_nr_need(index);
		prev_misfit_need = compute_prev_cluster_misfit_need(index);

		cluster->nrrun = nr_need + prev_misfit_need;
		cluster->max_nr = compute_cluster_max_nr(index);

		trace_core_ctl_update_nr_need(cluster->first_cpu, nr_need,
					prev_misfit_need,
					cluster->nrrun, cluster->max_nr);

		big_avg += cluster_real_big_tasks(index);
	}
	spin_unlock_irqrestore(&state_lock, flags);

	walt_rotation_checkpoint(big_avg);
}

#define MAX_NR_THRESHOLD	4
/* adjust needed CPUs based on current runqueue information */
static unsigned int apply_task_need(const struct cluster_data *cluster,
				    unsigned int new_need)
{
	/* unisolate all cores if there are enough tasks */
	if (cluster->nrrun >= cluster->task_thres)
		return cluster->num_cpus;

	/* only unisolate more cores if there are tasks to run */
	if (cluster->nrrun > new_need)
		new_need = new_need + 1;

	/*
	 * We don't want tasks to be overcrowded in a cluster.
	 * If any CPU has more than MAX_NR_THRESHOLD in the last
	 * window, bring another CPU to help out.
	 */
	if (cluster->max_nr > MAX_NR_THRESHOLD)
		new_need = new_need + 1;

	return new_need;
}

/* ======================= load based core count  ====================== */

static unsigned int apply_limits(const struct cluster_data *cluster,
				 unsigned int need_cpus)
{
	return min(max(cluster->min_cpus, need_cpus), cluster->max_cpus);
}

static unsigned int get_active_cpu_count(const struct cluster_data *cluster)
{
	return cluster->num_cpus -
				sched_isolate_count(&cluster->cpu_mask, true);
}

static bool adjustment_possible(const struct cluster_data *cluster,
							unsigned int need)
{
	return (need < cluster->active_cpus || (need > cluster->active_cpus &&
						cluster->nr_isolated_cpus));
}

static bool eval_need(struct cluster_data *cluster)
{
	unsigned long flags;
	struct cpu_data *c;
	unsigned int need_cpus = 0, last_need, thres_idx;
	int ret = 0;
	bool need_flag = false;
	unsigned int new_need;
	s64 now, elapsed;

	if (unlikely(!cluster->inited))
		return 0;

	spin_lock_irqsave(&state_lock, flags);

	if (cluster->boost || !cluster->enable) {
		need_cpus = cluster->max_cpus;
	} else {
		cluster->active_cpus = get_active_cpu_count(cluster);
		thres_idx = cluster->active_cpus ? cluster->active_cpus - 1 : 0;
		list_for_each_entry(c, &cluster->lru, sib) {
			bool old_is_busy = c->is_busy;

			if (c->busy >= cluster->busy_up_thres[thres_idx] ||
			    sched_cpu_high_irqload(c->cpu))
				c->is_busy = true;
			else if (c->busy < cluster->busy_down_thres[thres_idx])
				c->is_busy = false;

			trace_core_ctl_set_busy(c->cpu, c->busy, old_is_busy,
						c->is_busy);
			need_cpus += c->is_busy;
		}
		need_cpus = apply_task_need(cluster, need_cpus);
	}
	new_need = apply_limits(cluster, need_cpus);
	need_flag = adjustment_possible(cluster, new_need);

	last_need = cluster->need_cpus;
	now = ktime_to_ms(ktime_get());

	if (new_need > cluster->active_cpus) {
		ret = 1;
	} else {
		/*
		 * When there is no change in need and there are no more
		 * active CPUs than currently needed, just update the
		 * need time stamp and return.
		 */
		if (new_need == last_need && new_need == cluster->active_cpus) {
			cluster->need_ts = now;
			spin_unlock_irqrestore(&state_lock, flags);
			return 0;
		}

		elapsed =  now - cluster->need_ts;
		ret = elapsed >= cluster->offline_delay_ms;
	}

	if (ret) {
		cluster->need_ts = now;
		cluster->need_cpus = new_need;
	}
	trace_core_ctl_eval_need(cluster->first_cpu, last_need, new_need,
				 ret && need_flag);
	spin_unlock_irqrestore(&state_lock, flags);

	return ret && need_flag;
}

static void apply_need(struct cluster_data *cluster)
{
	if (eval_need(cluster))
		wake_up_core_ctl_thread(cluster);
}

/* ========================= core count enforcement ==================== */

static void wake_up_core_ctl_thread(struct cluster_data *cluster)
{
	unsigned long flags;

	spin_lock_irqsave(&cluster->pending_lock, flags);
	cluster->pending = true;
	spin_unlock_irqrestore(&cluster->pending_lock, flags);
	wake_up_process(cluster->core_ctl_thread);
}

static u64 core_ctl_check_timestamp;

int core_ctl_set_boost(bool boost)
{
	unsigned int index = 0;
	struct cluster_data *cluster;
	unsigned long flags;
	int ret = 0;
	bool boost_state_changed = false;

	if (unlikely(!initialized))
		return 0;

	spin_lock_irqsave(&state_lock, flags);
	for_each_cluster(cluster, index) {
		if (boost) {
			boost_state_changed = !cluster->boost;
			++cluster->boost;
		} else {
			if (!cluster->boost) {
				ret = -EINVAL;
				break;
			} else {
				--cluster->boost;
				boost_state_changed = !cluster->boost;
			}
		}
	}
	spin_unlock_irqrestore(&state_lock, flags);

	if (boost_state_changed) {
		index = 0;
		for_each_cluster(cluster, index)
			apply_need(cluster);
	}

	trace_core_ctl_set_boost(cluster->boost, ret);

	return ret;
}
EXPORT_SYMBOL(core_ctl_set_boost);

void core_ctl_check(u64 window_start)
{
	int cpu;
	struct cpu_data *c;
	struct cluster_data *cluster;
	unsigned int index = 0;
	unsigned long flags;

	if (unlikely(!initialized))
		return;

	if (window_start == core_ctl_check_timestamp)
		return;

	core_ctl_check_timestamp = window_start;

	spin_lock_irqsave(&state_lock, flags);
	for_each_possible_cpu(cpu) {

		c = &per_cpu(cpu_state, cpu);
		cluster = c->cluster;

		if (!cluster || !cluster->inited)
			continue;

		c->busy = sched_get_cpu_util(cpu);
	}
	spin_unlock_irqrestore(&state_lock, flags);

	update_running_avg();

	for_each_cluster(cluster, index) {
		if (eval_need(cluster))
			wake_up_core_ctl_thread(cluster);
	}
}

static void move_cpu_lru(struct cpu_data *cpu_data)
{
	unsigned long flags;

	spin_lock_irqsave(&state_lock, flags);
	list_del(&cpu_data->sib);
	list_add_tail(&cpu_data->sib, &cpu_data->cluster->lru);
	spin_unlock_irqrestore(&state_lock, flags);
}

static int isolation_cpuhp_state(unsigned int cpu,  bool online)
{
	struct cpu_data *state = &per_cpu(cpu_state, cpu);
	struct cluster_data *cluster = state->cluster;
	unsigned int need;
	bool do_wakeup = false, unisolated = false;
	unsigned long flags;

	if (unlikely(!cluster || !cluster->inited))
		return 0;

	if (online) {
		cluster->active_cpus = get_active_cpu_count(cluster);

		/*
		 * Moving to the end of the list should only happen in
		 * CPU_ONLINE and not on CPU_UP_PREPARE to prevent an
		 * infinite list traversal when thermal (or other entities)
		 * reject trying to online CPUs.
		 */
		move_cpu_lru(state);
	} else {
		/*
		 * We don't want to have a CPU both offline and isolated.
		 * So unisolate a CPU that went down if it was isolated by us.
		 */
		if (state->isolated_by_us) {
			sched_unisolate_cpu_unlocked(cpu);
			state->isolated_by_us = false;
			unisolated = true;
		}

		/* Move a CPU to the end of the LRU when it goes offline. */
		move_cpu_lru(state);

		state->busy = 0;
		cluster->active_cpus = get_active_cpu_count(cluster);
	}

	need = apply_limits(cluster, cluster->need_cpus);
	spin_lock_irqsave(&state_lock, flags);
	if (unisolated)
		cluster->nr_isolated_cpus--;
	do_wakeup = adjustment_possible(cluster, need);
	spin_unlock_irqrestore(&state_lock, flags);
	if (do_wakeup)
		wake_up_core_ctl_thread(cluster);

	return 0;
}

static int core_ctl_isolation_online_cpu(unsigned int cpu)
{
	return isolation_cpuhp_state(cpu, true);
}

static int core_ctl_isolation_dead_cpu(unsigned int cpu)
{
	return isolation_cpuhp_state(cpu, false);
}

/* ============================ init code ============================== */

static int __init core_ctl_init(void)
{
	cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
			"core_ctl/isolation:online",
			core_ctl_isolation_online_cpu, NULL);

	cpuhp_setup_state_nocalls(CPUHP_CORE_CTL_ISOLATION_DEAD,
			"core_ctl/isolation:dead",
			NULL, core_ctl_isolation_dead_cpu);

#ifdef CONFIG_SCHED_WALT
	for_each_cpu(cpu, &cpus) {
		int ret;
		const struct cpumask *cluster_cpus = cpu_coregroup_mask(cpu);

		ret = cluster_init(cluster_cpus);
		if (ret)
			pr_warn("unable to create core ctl group: %d\n", ret);
		cpumask_andnot(&cpus, &cpus, cluster_cpus);
	}
#endif

	initialized = true;
	return 0;
}

late_initcall(core_ctl_init);
