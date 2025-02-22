// SPDX-License-Identifier: GPL-2.0-only
/* fair.c
 *
 * Android Vendor Hook Support
 *
 * Copyright 2020 Google LLC
 */
#include <kernel/sched/sched.h>
#include <kernel/sched/pelt.h>
#include <trace/events/power.h>
#include <soc/google/exynos-dm.h>

#include "sched_priv.h"
#include "sched_events.h"
#include "../systrace.h"

#if IS_ENABLED(CONFIG_PIXEL_EM)
#include "../../include/pixel_em.h"
struct pixel_em_profile **vendor_sched_pixel_em_profile;
EXPORT_SYMBOL_GPL(vendor_sched_pixel_em_profile);
#endif

extern unsigned int vendor_sched_util_post_init_scale;
extern bool vendor_sched_npi_packing;

unsigned int sched_capacity_margin[CPU_NUM] = {
			[0 ... CPU_NUM-1] = DEF_UTIL_THRESHOLD };

struct vendor_group_property vg[VG_MAX];

extern struct vendor_group_list vendor_group_list[VG_MAX];

static DEFINE_PER_CPU(unsigned int, cpu_cur_freq);

extern inline unsigned int uclamp_none(enum uclamp_id clamp_id);

unsigned long schedutil_cpu_util_pixel_mod(int cpu, unsigned long util_cfs,
				 unsigned long max, enum schedutil_type type,
				 struct task_struct *p);
unsigned int map_scaling_freq(int cpu, unsigned int freq);

extern void rvh_uclamp_eff_get_pixel_mod(void *data, struct task_struct *p, enum uclamp_id clamp_id,
					 struct uclamp_se *uclamp_max, struct uclamp_se *uclamp_eff,
					 int *ret);

/*****************************************************************************/
/*                       Upstream Code Section                               */
/*****************************************************************************/
/*
 * This part of code is copied from Android common GKI kernel and unmodified.
 * Any change for these functions in upstream GKI would require extensive review
 * to make proper adjustment in vendor hook.
 */

static void attach_task(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_held(&rq->lock);

	BUG_ON(task_rq(p) != rq);
	activate_task(rq, p, ENQUEUE_NOCLOCK);
	check_preempt_curr(rq, p, 0);
}

/*
 * attach_one_task() -- attaches the task returned from detach_one_task() to
 * its new rq.
 */
static void attach_one_task(struct rq *rq, struct task_struct *p)
{
	struct rq_flags rf;

	rq_lock(rq, &rf);
	update_rq_clock(rq);
	attach_task(rq, p);
	rq_unlock(rq, &rf);
}

#if !IS_ENABLED(CONFIG_64BIT)
static inline u64 cfs_rq_last_update_time(struct cfs_rq *cfs_rq)
{
	u64 last_update_time_copy;
	u64 last_update_time;

	do {
		last_update_time_copy = cfs_rq->load_last_update_time_copy;
		smp_rmb();
		last_update_time = cfs_rq->avg.last_update_time;
	} while (last_update_time != last_update_time_copy);

	return last_update_time;
}
#else
static inline u64 cfs_rq_last_update_time(struct cfs_rq *cfs_rq)
{
	return cfs_rq->avg.last_update_time;
}
#endif

#if IS_ENABLED(CONFIG_UCLAMP_TASK)
static inline unsigned long uclamp_task_util(struct task_struct *p)
{
	return clamp(task_util_est(p),
		     uclamp_eff_value(p, UCLAMP_MIN),
		     uclamp_eff_value(p, UCLAMP_MAX));
}
#else
static inline unsigned long uclamp_task_util(struct task_struct *p)
{
	return task_util_est(p);
}
#endif

/* Runqueue only has SCHED_IDLE tasks enqueued */
static int sched_idle_rq(struct rq *rq)
{
	return unlikely(rq->nr_running == rq->cfs.idle_h_nr_running &&
			rq->nr_running);
}

#ifdef CONFIG_SMP
int sched_cpu_idle(int cpu)
{
	return sched_idle_rq(cpu_rq(cpu));
}
#endif

static inline bool within_margin(int value, int margin)
{
	return ((unsigned int)(value + margin - 1) < (2 * margin - 1));
}

static inline void update_load_add(struct load_weight *lw, unsigned long inc)
{
	lw->weight += inc;
	lw->inv_weight = 0;
}

#define WMULT_CONST	(~0U)
#define WMULT_SHIFT	32

static void __update_inv_weight(struct load_weight *lw)
{
	unsigned long w;

	if (likely(lw->inv_weight))
		return;

	w = scale_load_down(lw->weight);

	if (BITS_PER_LONG > 32 && unlikely(w >= WMULT_CONST))
		lw->inv_weight = 1;
	else if (unlikely(!w))
		lw->inv_weight = WMULT_CONST;
	else
		lw->inv_weight = WMULT_CONST / w;
}

static u64 __calc_delta(u64 delta_exec, unsigned long weight, struct load_weight *lw)
{
	u64 fact = scale_load_down(weight);
	int shift = WMULT_SHIFT;

	__update_inv_weight(lw);

	if (unlikely(fact >> 32)) {
		while (fact >> 32) {
			fact >>= 1;
			shift--;
		}
	}

	fact = mul_u32_u32(fact, lw->inv_weight);

	while (fact >> 32) {
		fact >>= 1;
		shift--;
	}

	return mul_u64_u32_shr(delta_exec, fact, shift);
}

static u64 __sched_period(unsigned long nr_running);

#define for_each_sched_entity(se) \
		for (; se; se = se->parent)

u64 sched_slice(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	u64 slice = __sched_period(cfs_rq->nr_running + !se->on_rq);

	for_each_sched_entity(se) {
		struct load_weight *load;
		struct load_weight lw;

		cfs_rq = cfs_rq_of(se);
		load = &cfs_rq->load;

		if (unlikely(!se->on_rq)) {
			lw = cfs_rq->load;

			update_load_add(&lw, se->load.weight);
			load = &lw;
		}
		slice = __calc_delta(slice, se->load.weight, load);
	}
	return slice;
}

static void set_next_buddy(struct sched_entity *se)
{
	if (entity_is_task(se) && unlikely(task_has_idle_policy(task_of(se))))
		return;

	for_each_sched_entity(se) {
		if (SCHED_WARN_ON(!se->on_rq))
			return;
		cfs_rq_of(se)->next = se;
	}
}

static inline struct task_group *css_tg(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct task_group, css) : NULL;
}

/*****************************************************************************/
/*                       New Code Section                                    */
/*****************************************************************************/
/*
 * This part of code is new for this kernel, which are mostly helper functions.
 */
bool get_prefer_high_cap(struct task_struct *p)
{
	return vg[get_vendor_group(p)].prefer_high_cap;
}

static inline bool get_task_spreading(struct task_struct *p)
{
	return vg[get_vendor_group(p)].task_spreading;
}

#if IS_ENABLED(CONFIG_USE_GROUP_THROTTLE)
static inline unsigned int get_task_group_throttle(struct task_struct *p)
{
	return vg[get_vendor_group(p)].group_throttle;
}

static inline unsigned int get_group_throttle(struct task_group *tg)
{
	return vg[get_vendor_task_group_struct(tg)->group].group_throttle;
}
#endif

void init_vendor_group_data(void)
{
	int i;

	for (i = 0; i < VG_MAX; i++) {
		INIT_LIST_HEAD(&vendor_group_list[i].list);
		raw_spin_lock_init(&vendor_group_list[i].lock);
		vendor_group_list[i].cur_iterator = NULL;
	}
}

#if defined(CONFIG_UCLAMP_TASK) && defined(CONFIG_FAIR_GROUP_SCHED)
static inline unsigned long cpu_util_cfs_group_mod_no_est(struct rq *rq)
{
	struct cfs_rq *cfs_rq, *pos;
	unsigned long util = 0, unclamped_util = 0;
	struct task_group *tg;
#if IS_ENABLED(CONFIG_USE_GROUP_THROTTLE)
	unsigned long scale_cpu = arch_scale_cpu_capacity(rq->cpu);
#endif

	// cpu_util_cfs = root_util - subgroup_util_sum + throttled_subgroup_util_sum
	for_each_leaf_cfs_rq_safe(rq, cfs_rq, pos) {
		if (&rq->cfs != cfs_rq) {
			tg = cfs_rq->tg;
			unclamped_util += cfs_rq->avg.util_avg;
#if IS_ENABLED(CONFIG_USE_GROUP_THROTTLE)
			util += min_t(unsigned long, READ_ONCE(cfs_rq->avg.util_avg),
				cap_scale(get_group_throttle(tg), scale_cpu));
#else
			util += READ_ONCE(cfs_rq->avg.util_avg);
#endif
		}
	}

	util += max_t(int, READ_ONCE(rq->cfs.avg.util_avg) - unclamped_util, 0);

	return util;
}

unsigned long cpu_util_cfs_group_mod(struct rq *rq)
{
	unsigned long util = cpu_util_cfs_group_mod_no_est(rq);

	if (sched_feat(UTIL_EST)) {
		// TODO: right now the limit of util_est is per task
		// consider to make it per group.
		util = max_t(unsigned long, util,
			     READ_ONCE(rq->cfs.avg.util_est.enqueued));
	}

	return util;
}
#else
#define cpu_util_cfs_group_mod cpu_util_cfs
#endif

unsigned long cpu_util(int cpu)
{
       struct rq *rq = cpu_rq(cpu);

       unsigned long util = cpu_util_cfs_group_mod(rq);

       return min_t(unsigned long, util, capacity_of(cpu));
}

#if IS_ENABLED(CONFIG_USE_GROUP_THROTTLE)
/* Similar to cpu_util_without but only count the task's group util contribution */
static unsigned long group_util_without(int cpu, struct task_struct *p, unsigned long max)
{
	unsigned long util = READ_ONCE(task_group(p)->cfs_rq[cpu]->avg.util_avg);

	if (cpu != task_cpu(p) || !READ_ONCE(p->se.avg.last_update_time))
		goto out;

	lsub_positive(&util, task_util(p));

out:
	return min_t(unsigned long, util, max);

}
#endif

static unsigned long cpu_util_without_raw(int cpu, struct task_struct *p)
{
	struct cfs_rq *cfs_rq;
	unsigned long util;

	/* Task has no contribution or is new */
	if (cpu != task_cpu(p) || !READ_ONCE(p->se.avg.last_update_time))
		return cpu_util_cfs_group_mod(cpu_rq(cpu));

	cfs_rq = &cpu_rq(cpu)->cfs;
	util = cpu_util_cfs_group_mod_no_est(cpu_rq(cpu));

	/* Discount task's util from CPU's util */
	lsub_positive(&util, task_util(p));

	/*
	 * Covered cases:
	 *
	 * a) if *p is the only task sleeping on this CPU, then:
	 *      cpu_util (== task_util) > util_est (== 0)
	 *    and thus we return:
	 *      cpu_util_without = (cpu_util - task_util) = 0
	 *
	 * b) if other tasks are SLEEPING on this CPU, which is now exiting
	 *    IDLE, then:
	 *      cpu_util >= task_util
	 *      cpu_util > util_est (== 0)
	 *    and thus we discount *p's blocked utilization to return:
	 *      cpu_util_without = (cpu_util - task_util) >= 0
	 *
	 * c) if other tasks are RUNNABLE on that CPU and
	 *      util_est > cpu_util
	 *    then we use util_est since it returns a more restrictive
	 *    estimation of the spare capacity on that CPU, by just
	 *    considering the expected utilization of tasks already
	 *    runnable on that CPU.
	 *
	 * Cases a) and b) are covered by the above code, while case c) is
	 * covered by the following code when estimated utilization is
	 * enabled.
	 */
	if (sched_feat(UTIL_EST)) {
		unsigned int estimated =
			READ_ONCE(cfs_rq->avg.util_est.enqueued);

		/*
		 * Despite the following checks we still have a small window
		 * for a possible race, when an execl's select_task_rq_fair()
		 * races with LB's detach_task():
		 *
		 *   detach_task()
		 *     p->on_rq = TASK_ON_RQ_MIGRATING;
		 *     ---------------------------------- A
		 *     deactivate_task()                   \
		 *       dequeue_task()                     + RaceTime
		 *         util_est_dequeue()              /
		 *     ---------------------------------- B
		 *
		 * The additional check on "current == p" it's required to
		 * properly fix the execl regression and it helps in further
		 * reducing the chances for the above race.
		 */
		if (unlikely(task_on_rq_queued(p) || current == p))
			lsub_positive(&estimated, _task_util_est(p));

		util = max_t(unsigned long, util, estimated);
	}

	return util;
}

struct vendor_group_property *get_vendor_group_property(enum vendor_group group)
{
	SCHED_WARN_ON(group < VG_SYSTEM || group >= VG_MAX);

	return &(vg[group]);
}

static bool task_fits_capacity(struct task_struct *p, int cpu,  bool sync_boost)
{
	unsigned long uclamp_min = uclamp_eff_value(p, UCLAMP_MIN);
	unsigned long uclamp_max = uclamp_eff_value(p, UCLAMP_MAX);
	unsigned long task_util = task_util_est(p);
	bool is_important = (get_prefer_idle(p) || uclamp_latency_sensitive(p)) 
                            && (uclamp_boosted(p) || get_prefer_high_cap(p));
    bool is_critical_task = is_important || sync_boost;

	if (cpu >= MAX_CAPACITY_CPU) {
		return true;
    }

    if (!is_critical_task && cpu < MID_CAPACITY_CPU) {
        return true;
    }

	if (is_critical_task && cpu < MID_CAPACITY_CPU) {
		return false;
    }

	/*
	 * Ignore uclamp if spreading the task
	 */
	if (get_task_spreading(p)) {
		uclamp_min = uclamp_none(UCLAMP_MIN);
		uclamp_max = uclamp_none(UCLAMP_MAX);
	}

#if IS_ENABLED(CONFIG_USE_GROUP_THROTTLE)
	/* clamp task utilization against its per-cpu group limit */
	task_util = min_t(unsigned long, task_util, cap_scale(get_task_group_throttle(p),
							arch_scale_cpu_capacity(cpu)));
#endif

	return rvh_util_fits_cpu(task_util, uclamp_min, uclamp_max, cpu);
}

static inline bool cpu_is_in_target_set(struct task_struct *p, int cpu) {
    bool is_important = (get_prefer_idle(p) || uclamp_latency_sensitive(p)) 
                            && (uclamp_boosted(p) || get_prefer_high_cap(p));
    int first_cpu, next_usable_cpu;
    
    first_cpu = (is_important) ? MID_CAPACITY_CPU : MIN_CAPACITY_CPU;

    next_usable_cpu = cpumask_next(first_cpu - 1, p->cpus_ptr);
    return cpu >= next_usable_cpu || next_usable_cpu >= nr_cpu_ids;
}

/**
 * cpu_is_idle - is a given CPU idle for enqueuing work.
 * @cpu: the CPU in question.
 *
 * Return: 1 if the CPU is currently idle. 0 otherwise.
 */
int cpu_is_idle(int cpu)
{
	if (available_idle_cpu(cpu) || sched_cpu_idle(cpu))
		return 1;

	return 0;
}

static unsigned long cpu_util_next(int cpu, struct task_struct *p, int dst_cpu)
{
	struct cfs_rq *cfs_rq, *pos;
	unsigned long util = 0, unclamped_util = 0;
	unsigned long util_est;
	struct task_group *tg;
	long delta = 0;
	struct rq *rq = cpu_rq(cpu);
#if IS_ENABLED(CONFIG_USE_GROUP_THROTTLE)
	unsigned long scale_cpu = arch_scale_cpu_capacity(cpu);
#endif

	if (task_cpu(p) == cpu && dst_cpu != cpu)
		delta = -task_util(p);
	else if (task_cpu(p) != cpu && dst_cpu == cpu)
		delta = task_util(p);

	// For leaf groups
	for_each_leaf_cfs_rq_safe(rq, cfs_rq, pos) {
		if (&rq->cfs != cfs_rq) {
			unsigned long group_util = 0;
			tg = cfs_rq->tg;

			if (p->se.cfs_rq->tg == tg)
				group_util = max_t(int, READ_ONCE(cfs_rq->avg.util_avg) + delta, 0);
			else
				group_util = READ_ONCE(cfs_rq->avg.util_avg);

			unclamped_util += READ_ONCE(cfs_rq->avg.util_avg);
#if IS_ENABLED(CONFIG_USE_GROUP_THROTTLE)
			util += min_t(unsigned long, group_util,
				cap_scale(get_group_throttle(tg), scale_cpu));
#else
			util += group_util;
#endif
		}
	}

	// For root group
	if (p->se.cfs_rq->tg == rq->cfs.tg)
		util = max_t(long, READ_ONCE(rq->cfs.avg.util_avg) - unclamped_util + util + delta,
				   0);
	else
		util = max_t(long, READ_ONCE(rq->cfs.avg.util_avg) - unclamped_util + util, 0);

	if (sched_feat(UTIL_EST)) {
		util_est = READ_ONCE(rq->cfs.avg.util_est.enqueued);

		if (dst_cpu == cpu)
			util_est += _task_util_est(p);

		util = max_t(unsigned long, util, util_est);
	}

	return min(util, capacity_of(cpu));
}

/* These CL enum values are used as indices into {old_}floor[cl] */
enum { CL1, CL2 };
struct cl0_const {
	/* These are the new cl0 floors set by cl1 and cl2 if @p migrates */
	unsigned int floor[2][CPU_NUM];
	unsigned int old_floor[2];
};

static unsigned int cl0_floor_map(int cl, unsigned int freq)
{
	int cpu;

	cpu = cl == CL1 ? MID_CAPACITY_CPU : MAX_CAPACITY_CPU;
	return exynos_dm_constraint_freq(cpu, freq);
}

static unsigned int cl0_floor(const struct cl0_const *cl0, int src, int dst)
{
#define cpu_is_cl1(cpu) ((cpu) >= MID_CAPACITY_CPU && (cpu) < MAX_CAPACITY_CPU)
#define cpu_is_cl2(cpu) ((cpu) >= MAX_CAPACITY_CPU)
	unsigned int cl1_floor, cl2_floor;

	/* If @p is moving to or from cl1, then use cl1's new cl0 floor */
	if (cpu_is_cl1(src) || cpu_is_cl1(dst))
		cl1_floor = cl0->floor[CL1][dst];
	else
		cl1_floor = cl0->old_floor[CL1];

	/* If @p is moving to or from cl2, then use cl2's new cl0 floor */
	if (cpu_is_cl2(src) || cpu_is_cl2(dst))
		cl2_floor = cl0->floor[CL2][dst];
	else
		cl2_floor = cl0->old_floor[CL2];

	/* Return the higher of the two floors between cl1 and cl2 */
	return max(cl1_floor, cl2_floor);
}

static unsigned long
em_cpu_energy_pixel_mod(struct em_perf_domain *pd, unsigned long max_util,
			unsigned long sum_util, struct cl0_const *cl0, int src,
			int dst)
{
	int i, cl, cpu = cpumask_first(to_cpumask(pd->cpus));
	unsigned long freq, scale_cpu;
	struct em_perf_state *ps;

	if (!sum_util) {
		if (cpu) {
			/* No cl0 constraint if this cluster has no busy time */
			cl = cpu < MAX_CAPACITY_CPU ? CL1 : CL2;
			cl0->floor[cl][dst] = 0;
		}
		return 0;
	}

#if IS_ENABLED(CONFIG_PIXEL_EM)
	{
		struct pixel_em_profile **profile_ptr_snapshot;
		profile_ptr_snapshot = READ_ONCE(vendor_sched_pixel_em_profile);
		if (profile_ptr_snapshot) {
			struct pixel_em_profile *profile = READ_ONCE(*profile_ptr_snapshot);
			if (profile) {
				struct pixel_em_cluster *cluster = profile->cpu_to_cluster[cpu];
				struct pixel_em_opp *max_opp;
				struct pixel_em_opp *opp;

				max_opp = &cluster->opps[cluster->num_opps - 1];

				freq = map_util_freq_pixel_mod(max_util,
							       max_opp->freq,
							       max_opp->capacity,
							       cpu);
				freq = map_scaling_freq(cpu, freq);

				for (i = 0; i < cluster->num_opps; i++) {
					opp = &cluster->opps[i];
					if (opp->freq >= freq)
						break;
				}

				return opp->cost * sum_util / max_opp->capacity;
			}
		}
	}
#endif

	scale_cpu = arch_scale_cpu_capacity(cpu);
	ps = &pd->table[pd->nr_perf_states - 1];
	freq = map_util_freq_pixel_mod(max_util, ps->frequency, scale_cpu, cpu);
	if (!cpu)
		/* Apply the cl0 floor when assessing cl0's energy */
		freq = max_t(unsigned int, cl0_floor(cl0, src, dst), freq);
	freq = map_scaling_freq(cpu, freq);

	for (i = 0; i < pd->nr_perf_states; i++) {
		ps = &pd->table[i];
		if (ps->frequency >= freq)
			break;
	}

	/* Update the cl0 floor if this is cl1 or cl2 (cpu != 0) */
	if (cpu) {
		cl = cpu < MAX_CAPACITY_CPU ? CL1 : CL2;
		cl0->floor[cl][dst] = cl0_floor_map(cl, ps->frequency);
	}
	return ps->cost * sum_util / scale_cpu;
}

struct em_calc {
	unsigned long energy_util;
	unsigned long cpu_util;
};

static void
calc_energy(struct em_calc *ec, struct task_struct *p, struct perf_domain *pd,
	    unsigned long cpu_cap, int cpu, int dst)
{
	unsigned int util_cfs;

	/*
	 * The capacity state of CPUs of the current rd can be driven by CPUs of
	 * another rd if they belong to the same performance domain. So, account
	 * for the utilization of these CPUs too by masking pd with
	 * cpu_online_mask instead of the rd span.
	 *
	 * If an entire performance domain is outside of the current rd, it will
	 * not appear in its pd list and will not be accounted.
	 */
	util_cfs = cpu_util_next(cpu, p, dst);

	/*
	 * Busy time computation: utilization clamping is not required since the
	 * ratio (sum_util / cpu_capacity) is already enough to scale the EM
	 * reported power consumption at the (eventually clamped) cpu_capacity.
	 */
	ec->energy_util = schedutil_cpu_util_pixel_mod(cpu, util_cfs, cpu_cap,
						       ENERGY_UTIL, NULL);

	/*
	 * Performance domain frequency: utilization clamping must be considered
	 * since it affects the selection of the performance domain frequency.
	 * NOTE: in case RT tasks are running, by default the FREQUENCY_UTIL's
	 * utilization can be max OPP.
	 */
	ec->cpu_util = schedutil_cpu_util_pixel_mod(cpu, util_cfs, cpu_cap,
						    FREQUENCY_UTIL,
						    cpu == dst ? p : NULL);
}

void rvh_enqueue_task_fair_pixel_mod(void *data, struct rq *rq, struct task_struct *p, int flags)
{
	/* Can only process uclamp after sched_slice() was updated */
	if (uclamp_is_used()) {
		if (uclamp_can_ignore_uclamp_max(rq, p)) {
			uclamp_set_ignore_uclamp_max(p);
			/* GKI has incremented it already, undo that */
			uclamp_rq_dec_id(rq, p, UCLAMP_MAX);
		}

		if (uclamp_can_ignore_uclamp_min(rq, p)) {
			uclamp_set_ignore_uclamp_min(p);
			/* GKI has incremented it already, undo that */
			uclamp_rq_dec_id(rq, p, UCLAMP_MIN);
		}
	}

	/*
	 * We strategically tell schedutil to ignore requests to update
	 * frequencies when we call rvh_set_iowait_pixel_mod().
	 *
	 * Now we have applied the uclamp filter, we'll unconditionally request
	 * a frequency update which should take all changes into account in one
	 * go.
	 */
	cpufreq_update_util(rq, SCHED_PIXEL_RESUME_UPDATES);
}

void rvh_dequeue_task_fair_pixel_mod(void *data, struct rq *rq, struct task_struct *p, int flags)
{
	/* Resetting uclamp filter is handled in dequeue_task() */
}

/*
 * @energy is expected to be initialized to zero for each CPU in @dst_mask.
 *
 * When @p moves to a new cluster, it affects power for both its old cluster and
 * new cluster. All CPUs in the source and destination clusters need to have
 * energy recomputed. But the energy calculation can be cached for any CPUs in
 * these clusters which are neither the source CPU nor destination CPU. If a CPU
 * is the one that @p moves off of, then it needs to have energy recomputed with
 * @p removed. If a CPU is the one that @p moves onto, then it needs to have
 * energy recomputed with @p added. But the other CPUs in the cluster only need
 * to have energy recomputed due to the effect of schedutil increasing CPU
 * frequency for the destination CPU cluster and/or decreasing CPU frequency for
 * the source CPU cluster. The other CPUs in the source and destination clusters
 * are otherwise unaffected, and thus their energy calculation can be cached.
 */
static void
compute_energy_change(struct task_struct *p, struct perf_domain *pd, int src,
		      const cpumask_t *dst_mask, unsigned long energy[CPU_NUM])
{
	/*
	 * cached_calc[0] is for when @p moves _to_ a given CPU's cluster.
	 * cached_calc[1] is for when @p moves _from_ a given CPU's cluster.
	 */
	static DEFINE_PER_CPU_ALIGNED(struct em_calc [2][CPU_NUM], cached_calc);
	cpumask_t *cmask, cached_mask[2] = {};
	struct em_calc *cache, *ec, tmp_ec;
	struct cl0_const cl0;
	bool from, no_cache;
	unsigned long cap;
	int cpu, dst;

	/* Get the old cl0 floor for cl1 and cl2 */
	cl0.old_floor[CL1] = cl0_floor_map(CL1, per_cpu(cpu_cur_freq,
							MID_CAPACITY_CPU));
	cl0.old_floor[CL2] = cl0_floor_map(CL2, per_cpu(cpu_cur_freq,
							MAX_CAPACITY_CPU));

	/*
	 * The pd list is assumed to have cl0's pd as the final pd. This is
	 * important in order to calculate cl0's floor constraint.
	 */
	for (; pd; pd = pd->next) {
		const cpumask_t *pd_mask = perf_domain_span(pd);

		/*
		 * The energy model mandates all the CPUs of a performance
		 * domain have the same capacity.
		 */
		cap = arch_scale_cpu_capacity(cpumask_first(pd_mask));

		/*
		 * The cache index is 0 if @p is moving to this cluster, and 1
		 * if @p is moving away from this cluster.
		 */
		from = cpumask_test_cpu(src, pd_mask);
		cache = this_cpu_ptr(cached_calc)[from];
		cmask = &cached_mask[from];

		/* Calculate energy for each CPU @dst if @p moves to @dst */
		for_each_cpu(dst, dst_mask) {
			unsigned long sum_util = 0;
			unsigned int max_util = 0;

			/* Compute @p's energy change for this cluster's CPUs */
			for_each_cpu_and(cpu, pd_mask, cpu_online_mask) {
				/*
				 * Calculate the effect of @p moving to or from
				 * this specific CPU. This calculation is unique
				 * for the source and destination CPU and thus
				 * cannot be cached. This is O(2*CPU_NUM).
				 *
				 * Otherwise, calculate the effect of @p's
				 * migration to @dst on this CPU that's neither
				 * the source nor destination CPU, but is part
				 * of the source or destination cluster. It is
				 * therefore affected by CPU frequency changes
				 * for its cluster. This calculation can be
				 * cached, as mentioned in the large comment
				 * above. This is also O(2*CPU_NUM).
				 */
				no_cache = cpu == dst || cpu == src;
				ec = no_cache ? &tmp_ec : &cache[cpu];
				if (no_cache || !cpumask_test_cpu(cpu, cmask)) {
					/* Get @cpu's energy if @p is on @dst */
					calc_energy(ec, p, pd, cap, cpu, dst);
					if (!no_cache)
						__cpumask_set_cpu(cpu, cmask);
				}

				sum_util += ec->energy_util;
				if (ec->cpu_util > max_util)
					max_util = ec->cpu_util;
			}

			/* Add in this cluster's energy impact for @p on @dst */
			energy[dst] += em_cpu_energy_pixel_mod(pd->em_pd,
							       max_util,
							       sum_util, &cl0,
							       src, dst);
		}
	}
}

#if IS_ENABLED(CONFIG_USE_GROUP_THROTTLE)
/* If a task_group is over its group limit on a particular CPU with margin considered */
static inline bool group_overutilized(int cpu, struct task_group *tg)
{

	unsigned long group_capacity = cap_scale(get_group_throttle(tg),
					arch_scale_cpu_capacity(cpu));
	unsigned long group_util = READ_ONCE(tg->cfs_rq[cpu]->avg.util_avg);
	return cpu_overutilized(group_util, group_capacity, cpu);
}
#endif

/*****************************************************************************/
/*                       Modified Code Section                               */
/*****************************************************************************/
/*
 * This part of code is vendor hook functions, which modify or extend the original
 * functions.
 */

/*
 * To avoid export more and more sysctl from GKI kernel,
 * use following setting directly dumped from running Android
 * and also assume the setting is not changed in runtime.
 *
 * sysctl_sched
 * .sysctl_sched_latency                    : 10.000000
 * .sysctl_sched_min_granularity            : 3.000000
 * .sysctl_sched_wakeup_granularity         : 2.000000
 * .sysctl_sched_child_runs_first           : 0
 * .sysctl_sched_features                   : 33477435
 * .sysctl_sched_tunable_scaling            : 0 (none)
 */
#define sysctl_sched_min_granularity 3000000ULL

static u64 __sched_period(unsigned long nr_running)
{
	unsigned int sched_nr_latency = DIV_ROUND_UP(sysctl_sched_latency,
					sysctl_sched_min_granularity);

	if (unlikely(nr_running > sched_nr_latency))
		return nr_running * sysctl_sched_min_granularity;
	else
		return sysctl_sched_latency;
}

static bool cpu_is_better(int cpu, int best_cpu,
			  const unsigned int exit_lat[CPU_NUM],
			  unsigned long util, unsigned long l_util)
{
	/*
	 * Find the CPU with the lowest raw utilization ratio. A non-idle CPU or
	 * CPU with lower exit latency is preferred when utilization is equal.
	 */
	return util < l_util || (util == l_util &&
				 exit_lat[cpu] < exit_lat[best_cpu]);
}

static unsigned long cpu_util_ratio(struct task_struct *p,
				    const unsigned long cap[CPU_NUM],
				    const unsigned int exit_lat[CPU_NUM],
				    int cpu, int prev_cpu, int *best_cpu,
				    unsigned long *l_util)
{
	unsigned long util;

	/* Exclude @p from the CPU's utilization if this is the previous CPU */
	if (cpu == prev_cpu)
		util = cpu_util_without_raw(cpu, p);
	else
		util = cpu_util_cfs_group_mod(cpu_rq(cpu));
	util = util * SCHED_CAPACITY_SCALE / cap[cpu];
	if (cpu_is_better(cpu, *best_cpu, exit_lat, util, *l_util)) {
		*l_util = util;
		*best_cpu = cpu;
	}

	return util;
}

/* UPSTREAM UCLAMP CODE - start */
static inline unsigned int
uclamp_idle_value(struct rq *rq, enum uclamp_id clamp_id,
		  unsigned int clamp_value)
{
	/*
	 * Avoid blocked utilization pushing up the frequency when we go
	 * idle (which drops the max-clamp) by retaining the last known
	 * max-clamp.
	 */
	if (clamp_id == UCLAMP_MAX) {
		rq->uclamp_flags |= UCLAMP_FLAG_IDLE;
		return clamp_value;
	}

	return uclamp_none(UCLAMP_MIN);
}

static inline void uclamp_idle_reset(struct rq *rq, enum uclamp_id clamp_id,
				     unsigned int clamp_value)
{
	/* Reset max-clamp retention only on idle exit */
	if (!(rq->uclamp_flags & UCLAMP_FLAG_IDLE))
		return;

	WRITE_ONCE(rq->uclamp[clamp_id].value, clamp_value);
}

static inline struct uclamp_se
uclamp_tg_restrict(struct task_struct *p, enum uclamp_id clamp_id)
{
	struct uclamp_se uc_req = p->uclamp_req[clamp_id];
#ifdef CONFIG_UCLAMP_TASK_GROUP
	struct uclamp_se uc_max;

	/*
	 * Tasks in autogroups or root task group will be
	 * restricted by system defaults.
	 */
	if (task_group_is_autogroup(task_group(p)))
		return uc_req;
	if (task_group(p) == &root_task_group)
		return uc_req;

	uc_max = task_group(p)->uclamp[clamp_id];
	if (uc_req.value > uc_max.value || !uc_req.user_defined)
		return uc_max;
#endif

	return uc_req;
}

/*
 * The effective clamp bucket index of a task depends on, by increasing
 * priority:
 * - the task specific clamp value, when explicitly requested from userspace
 * - the task group effective clamp value, for tasks not either in the root
 *   group or in an autogroup
 * - the system default clamp value, defined by the sysadmin
 */
static inline struct uclamp_se
uclamp_eff_get(struct task_struct *p, enum uclamp_id clamp_id)
{
	struct uclamp_se uc_req = uclamp_tg_restrict(p, clamp_id);
	struct uclamp_se uc_max = uclamp_default[clamp_id];
	struct uclamp_se uc_eff;
	int ret = 0;

	// Instead of calling trace_android_*, call vendor func directly
	rvh_uclamp_eff_get_pixel_mod(NULL, p, clamp_id, &uc_max, &uc_eff, &ret);
	if (ret)
		return uc_eff;

	/* System default restrictions always apply */
	if (unlikely(uc_req.value > uc_max.value))
		return uc_max;

	return uc_req;
}

static inline
unsigned int uclamp_rq_max_value(struct rq *rq, enum uclamp_id clamp_id,
				   unsigned int clamp_value)
{
	struct uclamp_bucket *bucket = rq->uclamp[clamp_id].bucket;
	int bucket_id = UCLAMP_BUCKETS - 1;

	/*
	 * Since both min and max clamps are max aggregated, find the
	 * top most bucket with tasks in.
	 */
	for ( ; bucket_id >= 0; bucket_id--) {
		if (!bucket[bucket_id].tasks)
			continue;
		return bucket[bucket_id].value;
	}

	/* No tasks -- default clamp values */
	return uclamp_idle_value(rq, clamp_id, clamp_value);
}


/*
 * When a task is enqueued on a rq, the clamp bucket currently defined by the
 * task's uclamp::bucket_id is refcounted on that rq. This also immediately
 * updates the rq's clamp value if required.
 *
 * Tasks can have a task-specific value requested from user-space, track
 * within each bucket the maximum value for tasks refcounted in it.
 * This "local max aggregation" allows to track the exact "requested" value
 * for each bucket when all its RUNNABLE tasks require the same clamp.
 */
inline void uclamp_rq_inc_id(struct rq *rq, struct task_struct *p,
			     enum uclamp_id clamp_id)
{
	struct uclamp_rq *uc_rq = &rq->uclamp[clamp_id];
	struct uclamp_se *uc_se = &p->uclamp[clamp_id];
	struct uclamp_bucket *bucket;

	lockdep_assert_held(&rq->lock);

	if(SCHED_WARN_ON(!uclamp_is_used()))
		return;

	/* Update task effective clamp */
	p->uclamp[clamp_id] = uclamp_eff_get(p, clamp_id);

	bucket = &uc_rq->bucket[uc_se->bucket_id];
	bucket->tasks++;
	uc_se->active = true;

	uclamp_idle_reset(rq, clamp_id, uc_se->value);

	/*
	 * Local max aggregation: rq buckets always track the max
	 * "requested" clamp value of its RUNNABLE tasks.
	 */
	if (bucket->tasks == 1 || uc_se->value > bucket->value)
		bucket->value = uc_se->value;

	if (uc_se->value > READ_ONCE(uc_rq->value))
		WRITE_ONCE(uc_rq->value, uc_se->value);
}

/*
 * When a task is dequeued from a rq, the clamp bucket refcounted by the task
 * is released. If this is the last task reference counting the rq's max
 * active clamp value, then the rq's clamp value is updated.
 *
 * Both refcounted tasks and rq's cached clamp values are expected to be
 * always valid. If it's detected they are not, as defensive programming,
 * enforce the expected state and warn.
 */
inline void uclamp_rq_dec_id(struct rq *rq, struct task_struct *p,
			     enum uclamp_id clamp_id)
{
	struct uclamp_rq *uc_rq = &rq->uclamp[clamp_id];
	struct uclamp_se *uc_se = &p->uclamp[clamp_id];
	struct uclamp_bucket *bucket;
	unsigned int bkt_clamp;
	unsigned int rq_clamp;

	lockdep_assert_held(&rq->lock);

    if(SCHED_WARN_ON(!uclamp_is_used()))
		    return;

	/*
	 * If sched_uclamp_used was enabled after task @p was enqueued,
	 * we could end up with unbalanced call to uclamp_rq_dec_id().
	 *
	 * In this case the uc_se->active flag should be false since no uclamp
	 * accounting was performed at enqueue time and we can just return
	 * here.
	 *
	 * Need to be careful of the following enqeueue/dequeue ordering
	 * problem too
	 *
	 *	enqueue(taskA)
	 *	// sched_uclamp_used gets enabled
	 *	enqueue(taskB)
	 *	dequeue(taskA)
	 *	// Must not decrement bukcet->tasks here
	 *	dequeue(taskB)
	 *
	 * where we could end up with stale data in uc_se and
	 * bucket[uc_se->bucket_id].
	 *
	 * The following check here eliminates the possibility of such race.
	 */
	if (unlikely(!uc_se->active))
		return;

	bucket = &uc_rq->bucket[uc_se->bucket_id];

	SCHED_WARN_ON(!bucket->tasks);
	if (likely(bucket->tasks))
		bucket->tasks--;

	uc_se->active = false;

	/*
	 * Keep "local max aggregation" simple and accept to (possibly)
	 * overboost some RUNNABLE tasks in the same bucket.
	 * The rq clamp bucket value is reset to its base value whenever
	 * there are no more RUNNABLE tasks refcounting it.
	 */
	if (likely(bucket->tasks))
		return;

	rq_clamp = READ_ONCE(uc_rq->value);
	/*
	 * Defensive programming: this should never happen. If it happens,
	 * e.g. due to future modification, warn and fixup the expected value.
	 */
	SCHED_WARN_ON(bucket->value > rq_clamp);
	if (bucket->value >= rq_clamp) {
		bkt_clamp = uclamp_rq_max_value(rq, clamp_id, uc_se->value);
		WRITE_ONCE(uc_rq->value, bkt_clamp);
	}
}
/* UPSTREAM UCLAMP CODE - end */

static int find_energy_efficient_cpu(struct task_struct *p, int prev_cpu,
				     const cpumask_t *valid_mask)
{
	unsigned long cap[CPU_NUM], cpu_util[CPU_NUM], energy[CPU_NUM] = {};
	unsigned long l_util = ULONG_MAX, p_util;
	cpumask_t allowed, candidates = {};
	struct cpuidle_state *idle_state;
	unsigned int exit_lat[CPU_NUM];
	struct perf_domain *pd;
	int i, best_cpu;
	struct rq *rq;

	/*
	 * If there aren't any valid CPUs which are active, then just return the
	 * first valid CPU since it's possible for certain types of tasks to run
	 * on !active CPUs.
	 */
	if (unlikely(!cpumask_and(&allowed, valid_mask, cpu_active_mask)))
		return cpumask_first(valid_mask);

	/* Compute the utilization for this task */
	p_util = get_task_spreading(p) ? task_util_est(p) : uclamp_task_util(p);

	/*
	 * Find the best-fitting CPU with the lowest total raw utilization
	 * ratio; i.e., the least relatively-loaded CPU. Note that although
	 * idle_get_state() requires an RCU read lock, an RCU read lock isn't
	 * needed because we're not preemptible and RCU-sched is unified with
	 * normal RCU. Therefore, non-preemptible contexts are implicitly
	 * RCU-safe.
	 *
	 * Iteration through @allowed is intended to go from the lowest-capacity
	 * cluster to the highest-capacity cluster in order to pack tasks onto
	 * lower-capacity clusters. If all cores in a higher-capacity cluster
	 * can idle, then it may be possible to enter a cluster idle state where
	 * the whole cluster goes into a deeper C-state, saving more power. This
	 * is generally moot for the lowest-capacity cluster though, since it
	 * typically contains the boot CPU and handles housekeeping, plus
	 * generally has the most cores, so it's less likely for it to enter
	 * cluster idle.
	 *
	 * Packing tasks onto lower-capacity clusters also improves overall
	 * single-threaded performance by reducing the load on higher-capacity
	 * CPUs, making them more available to heavy tasks.
	 */
	for_each_cpu(i, &allowed) {
		/*
		 * Get the current capacity of this CPU adjusted for thermal
		 * pressure as well as IRQ and RT-task time.
		 */
		cap[i] = capacity_of(i);

		/* Get the idle exit latency for this CPU if it's idle */
		rq = cpu_rq(i);
		idle_state = idle_get_state(rq);
		exit_lat[i] = idle_state ? idle_state->exit_latency : 0;

		/* Calculate the raw utilization ratio if this CPU fits */
		if (!cpu_overutilized(p_util, cap[i], i)) {
			cpu_util[i] = cpu_util_ratio(p, cap, exit_lat, i,
						     prev_cpu, &best_cpu,
						     &l_util);
			__cpumask_set_cpu(i, &candidates);
		}
	}

	/* If no CPU fits, then place the task on the minimum capacity CPU */
	if (l_util == ULONG_MAX)
		return MIN_CAPACITY_CPU;

	/* Stop now if only one CPU fits */
	if (cpumask_weight(&candidates) == 1)
		return best_cpu;

	/*
	 * Quickly filter out CPUs with significantly higher utilization by
	 * comparing floor(sqrt(util)) for each candidate. This helps avoid
	 * CPUs which are quadratically more loaded than the least utilized CPU
	 * found earlier, and eliminates their heavy energy computations. These
	 * CPUs are a bad choice from a performance standpoint, so discard them.
	 */
	l_util = int_sqrt(l_util);
	__cpumask_clear_cpu(best_cpu, &candidates);
	for_each_cpu(i, &candidates) {
		if (int_sqrt(cpu_util[i]) > l_util)
			__cpumask_clear_cpu(i, &candidates);
	}

	/* Stop now if all other CPUs are obviously a bad choice */
	if (cpumask_empty(&candidates))
		return best_cpu;

	pd = rcu_dereference(rq->rd->pd);
	if (unlikely(!pd))
		goto check_prev;

	/* Compute energy with @best_cpu included, and then take it back out */
	__cpumask_set_cpu(best_cpu, &candidates);
	compute_energy_change(p, pd, prev_cpu, &candidates, energy);
	__cpumask_clear_cpu(best_cpu, &candidates);

	/*
	 * Search for an energy efficient alternative to @best_cpu. This
	 * intentionally iterates over the candidates in ascending order from
	 * the lowest-capacity cluster to the highest-capacity cluster; that
	 * way, CPUs from lower-capacity clusters are preferred when there are
	 * multiple CPU candidates available that have similar energy and
	 * performance attributes.
	 */
	energy[best_cpu] = int_sqrt(energy[best_cpu]);
	for_each_cpu(i, &candidates) {
		/*
		 * Compare floor(sqrt(energy)) to ignore small differences in
		 * energy and prefer performance at the expense of slightly
		 * higher predicted energy. This also helps avoid bouncing tasks
		 * between different CPUs over very small energy differences,
		 * which hurts performance and can worsen energy.
		 */
		energy[i] = int_sqrt(energy[i]);
		if (energy[i] > energy[best_cpu])
			continue;

		/*
		 * Use this CPU if it has either lower energy or equal energy
		 * with better performance.
		 */
		if (energy[i] < energy[best_cpu] ||
		    cpu_is_better(i, best_cpu, exit_lat, cpu_util[i],
				  cpu_util[best_cpu]))
			best_cpu = i;
	}

check_prev:
	/*
	 * If utilization, idle exit latency, and energy are equal between the
	 * previous CPU and the best CPU, prefer the previous CPU if it's part
	 * of the same cluster as the best CPU or a lower-capacity cluster. The
	 * previous CPU isn't preferred if it's part of a higher-capacity
	 * cluster in order to pack tasks into lower-capacity clusters.
	 *
	 * This check is at the end because there's no way to know which cluster
	 * the best CPU will belong to until the final best CPU is found.
	 */
	if (cpumask_test_cpu(prev_cpu, &candidates) &&
	    cpu_util[prev_cpu] == cpu_util[best_cpu] &&
	    exit_lat[prev_cpu] == exit_lat[best_cpu] &&
	    energy[prev_cpu] == energy[best_cpu] &&
	    capacity_orig_of(prev_cpu) <= capacity_orig_of(best_cpu))
		best_cpu = prev_cpu;

	return best_cpu;
}

void vh_arch_set_freq_scale_pixel_mod(void *data, const struct cpumask *cpus,
				      unsigned long freq,
				      unsigned long max, unsigned long *scale)
{
#if IS_ENABLED(CONFIG_PIXEL_EM)
	int i;
	struct pixel_em_profile **profile_ptr_snapshot;
	profile_ptr_snapshot = READ_ONCE(vendor_sched_pixel_em_profile);
	if (profile_ptr_snapshot) {
		struct pixel_em_profile *profile = READ_ONCE(*profile_ptr_snapshot);
		if (profile) {
			struct pixel_em_cluster *cluster;
			struct pixel_em_opp *max_opp;
			struct pixel_em_opp *opp;

			cluster = profile->cpu_to_cluster[cpumask_first(cpus)];
			max_opp = &cluster->opps[cluster->num_opps - 1];

			for (i = 0; i < cluster->num_opps; i++) {
				opp = &cluster->opps[i];
				if (opp->freq >= freq)
					break;
			}

			*scale = (opp->capacity << SCHED_CAPACITY_SHIFT) /
				  max_opp->capacity;
		}
	}
#endif
	per_cpu(cpu_cur_freq, cpumask_first(cpus)) = freq;
}
EXPORT_SYMBOL_GPL(vh_arch_set_freq_scale_pixel_mod);

void rvh_set_iowait_pixel_mod(void *data, struct task_struct *p, int *should_iowait_boost)
{
	bool is_important = (get_prefer_idle(p) || uclamp_latency_sensitive(p)) && (get_prefer_high_cap(p) || uclamp_boosted(p));
	*should_iowait_boost = p->in_iowait && is_important;

	/*
	 * Tell sched pixel to ignore cpufreq updates. this happens at
	 * enqueue_task_fair() entry.
	 *
	 * We want to defer all request to defer frequency updates until uclamp
	 * filter is applied.
	 *
	 * Note that enqueue_task_fair() could request cpufreq updates when
	 * calling update_load_avg(). Since this vh is called before those
	 * - this strategic block will ensure all subsequent requests are
	 *   ignored.
	 */
	cpufreq_update_util(task_rq(p), SCHED_PIXEL_BLOCK_UPDATES);
}

void rvh_cpu_overutilized_pixel_mod(void *data, int cpu, int *overutilized)
{
	unsigned long rq_util_min = uclamp_rq_get(cpu_rq(cpu), UCLAMP_MIN);
	unsigned long rq_util_max = uclamp_rq_get(cpu_rq(cpu), UCLAMP_MAX);

	*overutilized = !rvh_util_fits_cpu(cpu_util(cpu), rq_util_min, rq_util_max, cpu);
}

unsigned long map_util_freq_pixel_mod(unsigned long util, unsigned long freq,
				      unsigned long cap, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long rq_util = cpu_util(cpu) + cpu_util_rt(rq);
	unsigned long uclamp_min = rq->uclamp[UCLAMP_MIN].value;
	unsigned long uclamp_max = rq->uclamp[UCLAMP_MAX].value;

	/* if uclamp_min boosted, don't apply any additional boost */
	if (rq_util < uclamp_min)
		return freq * util / cap;

	util = util * get_sched_capacity_margin(cpu) >> SCHED_CAPACITY_SHIFT;

	/* Don't allow the boost to go beyond uclamp_max */
	util = util >= uclamp_max ? uclamp_max : util;

	return freq * util / cap;
}

static inline struct uclamp_se
uclamp_tg_restrict_pixel_mod(struct task_struct *p, enum uclamp_id clamp_id)
{
	struct uclamp_se uc_req = p->uclamp_req[clamp_id];
	struct vendor_task_struct *vp = get_vendor_task_struct(p);
	struct vendor_binder_task_struct *vbinder = get_vendor_binder_task_struct(p);

#ifdef CONFIG_UCLAMP_TASK_GROUP
	unsigned int tg_min, tg_max, vnd_min, vnd_max, value;

	// Task group restriction
	tg_min = task_group(p)->uclamp[UCLAMP_MIN].value;
	tg_max = task_group(p)->uclamp[UCLAMP_MAX].value;
	// Vendor group restriction
	vnd_min = vg[vp->group].uc_req[UCLAMP_MIN].value;
	vnd_max = vg[vp->group].uc_req[UCLAMP_MAX].value;

	value = uc_req.value;
	value = clamp(value, max(tg_min, vnd_min),  min(tg_max, vnd_max));

	// Inherited uclamp restriction
	if (vbinder->active)
		value = clamp(value, vbinder->uclamp[UCLAMP_MIN], vbinder->uclamp[UCLAMP_MAX]);

	// RT_mutex inherited uclamp restriction
	value = clamp(value, vp->uclamp_pi[UCLAMP_MIN], vp->uclamp_pi[UCLAMP_MAX]);

	// For uclamp min, if task has a valid per-task setting that is lower than or equal to its
	// group value, increase the final uclamp value by 1. This would have effect only on
	// importance metrics which is used in task placement, and little effect on cpufreq.
	if (clamp_id == UCLAMP_MIN && uc_req.value <= max(tg_min, vnd_min) && uc_req.user_defined
		&& value < SCHED_CAPACITY_SCALE)
		value = value + 1;

	// For low prio unthrottled task, reduce its uclamp.max by 1 which
	// would affect task importance in cpu_rq thus affect task placement.
	// It should have no effect in cpufreq.
	if (clamp_id == UCLAMP_MAX && p->prio > DEFAULT_PRIO)
		value = min_t(unsigned int, UCLAMP_BUCKET_DELTA * (UCLAMP_BUCKETS - 1) - 1, value);

	uc_req.value = value;
	uc_req.bucket_id = get_bucket_id(value);
#endif

	return uc_req;
}

void rvh_uclamp_eff_get_pixel_mod(void *data, struct task_struct *p, enum uclamp_id clamp_id,
				  struct uclamp_se *uclamp_max, struct uclamp_se *uclamp_eff,
				  int *ret)
{
	struct uclamp_se uc_req;

	*ret = 1;

	uc_req = uclamp_tg_restrict_pixel_mod(p, clamp_id);

	/* System default restrictions always apply */
	if (unlikely(uc_req.value > uclamp_max->value)) {
		*uclamp_eff = *uclamp_max;
		return;
	}

	*uclamp_eff = uc_req;
	return;
}

void initialize_vendor_group_property(void)
{
	int i;
	unsigned int min_val = 0;
	unsigned int max_val = SCHED_CAPACITY_SCALE;

	for (i = 0; i < VG_MAX; i++) {
		vg[i].prefer_idle = false;
		vg[i].prefer_high_cap = false;
		vg[i].task_spreading = false;
		vg[i].group_throttle = max_val;
		vg[i].uc_req[UCLAMP_MIN].value = min_val;
		vg[i].uc_req[UCLAMP_MIN].bucket_id = get_bucket_id(min_val);
		vg[i].uc_req[UCLAMP_MIN].user_defined = false;
		vg[i].uc_req[UCLAMP_MAX].value = max_val;
		vg[i].uc_req[UCLAMP_MAX].bucket_id = get_bucket_id(max_val);
		vg[i].uc_req[UCLAMP_MAX].user_defined = false;
	}
}

void rvh_check_preempt_wakeup_pixel_mod(void *data, struct rq *rq, struct task_struct *p,
			bool *preempt, bool *nopreempt, int wake_flags, struct sched_entity *se,
			struct sched_entity *pse, int next_buddy_marked, unsigned int granularity)
{
	unsigned long ideal_runtime, delta_exec;

	if (entity_is_task(pse) || entity_is_task(se))
		return;

	ideal_runtime = sched_slice(cfs_rq_of(se), se);
	delta_exec = se->sum_exec_runtime - se->prev_sum_exec_runtime;
	/*
	 * If the current group has run enough time for its slice and the new
	 * group has bigger weight, go ahead and preempt.
	 */
	if (ideal_runtime <= delta_exec && se->load.weight < pse->load.weight) {
		if (!next_buddy_marked)
			set_next_buddy(pse);

		*preempt = true;
	}

}

void rvh_util_est_update_pixel_mod(void *data, struct cfs_rq *cfs_rq, struct task_struct *p,
				    bool task_sleep, int *ret)
{
	long last_ewma_diff;
	struct util_est ue;
	int cpu;
	unsigned long scale_cpu;

	*ret = 1;

	if (!sched_feat(UTIL_EST))
		return;

	/*
	 * Skip update of task's estimated utilization when the task has not
	 * yet completed an activation, e.g. being migrated.
	 */
	if (!task_sleep)
		return;

	/*
	 * If the PELT values haven't changed since enqueue time,
	 * skip the util_est update.
	 */
	ue = p->se.avg.util_est;
	if (ue.enqueued & UTIL_AVG_UNCHANGED)
		return;

	/*
	 * Reset EWMA on utilization increases, the moving average is used only
	 * to smooth utilization decreases.
	 */
	ue.enqueued = task_util(p);

	cpu = cpu_of(rq_of(cfs_rq));
	scale_cpu = arch_scale_cpu_capacity(cpu);
	// TODO: make util_est to sub cfs-rq and aggregate.
#ifdef CONFIG_UCLAMP_TASK
	// Currently util_est is done only in the root group
	// Current solution apply the clamp in the per-task level for simplicity.
	// However it may
	// 1) over grow by the group limit
	// 2) out of sync when task migrated between cgroups (cfs_rq)
	ue.enqueued = min((unsigned long)ue.enqueued, uclamp_eff_value(p, UCLAMP_MAX));
#if IS_ENABLED(CONFIG_USE_GROUP_THROTTLE)
	ue.enqueued = min_t(unsigned long, ue.enqueued,
			cap_scale(get_group_throttle(task_group(p)), scale_cpu));
#endif
#endif

	if (sched_feat(UTIL_EST_FASTUP)) {
		if (ue.ewma < ue.enqueued) {
			ue.ewma = ue.enqueued;
			goto done;
		}
	}

	/*
	 * Skip update of task's estimated utilization when its EWMA is
	 * already ~1% close to its last activation value.
	 */
	last_ewma_diff = ue.enqueued - ue.ewma;
	if (within_margin(last_ewma_diff, (SCHED_CAPACITY_SCALE / 100)))
		return;

	/*
	 * To avoid overestimation of actual task utilization, skip updates if
	 * we cannot grant there is idle time in this CPU.
	 */

	if (task_util(p) > capacity_orig_of(cpu))
		return;

	/*
	 * Update Task's estimated utilization
	 *
	 * When *p completes an activation we can consolidate another sample
	 * of the task size. This is done by storing the current PELT value
	 * as ue.enqueued and by using this value to update the Exponential
	 * Weighted Moving Average (EWMA):
	 *
	 *  ewma(t) = w *  task_util(p) + (1-w) * ewma(t-1)
	 *          = w *  task_util(p) +         ewma(t-1)  - w * ewma(t-1)
	 *          = w * (task_util(p) -         ewma(t-1)) +     ewma(t-1)
	 *          = w * (      last_ewma_diff            ) +     ewma(t-1)
	 *          = w * (last_ewma_diff  +  ewma(t-1) / w)
	 *
	 * Where 'w' is the weight of new samples, which is configured to be
	 * 0.25, thus making w=1/4 ( >>= UTIL_EST_WEIGHT_SHIFT)
	 */
	ue.ewma <<= UTIL_EST_WEIGHT_SHIFT;
	ue.ewma  += last_ewma_diff;
	ue.ewma >>= UTIL_EST_WEIGHT_SHIFT;
#ifdef CONFIG_UCLAMP_TASK
	ue.ewma = min((unsigned long)ue.ewma, uclamp_eff_value(p, UCLAMP_MAX));
#if IS_ENABLED(CONFIG_USE_GROUP_THROTTLE)
	ue.ewma = min_t(unsigned long, ue.ewma,
			cap_scale(get_group_throttle(task_group(p)), scale_cpu));
#endif
#endif
done:
	ue.enqueued |= UTIL_AVG_UNCHANGED;
	WRITE_ONCE(p->se.avg.util_est, ue);

	trace_sched_util_est_se_tp(&p->se);
}

void rvh_post_init_entity_util_avg_pixel_mod(void *data, struct sched_entity *se)
{
	struct cfs_rq *cfs_rq = cfs_rq_of(se);
	struct sched_avg *sa = &se->avg;
	long cpu_scale = arch_scale_cpu_capacity(cpu_of(rq_of(cfs_rq)));

	if (cfs_rq->avg.util_avg == 0) {
		sa->util_avg = vendor_sched_util_post_init_scale * cpu_scale / SCHED_CAPACITY_SCALE;
		sa->runnable_avg = sa->util_avg;
	}
}

void rvh_cpu_cgroup_online_pixel_mod(void *data, struct cgroup_subsys_state *css)
{
	struct vendor_task_group_struct *vtg;
	const char *name = css_tg(css)->css.cgroup->kn->name;

	vtg = get_vendor_task_group_struct(css_tg(css));

	if (strcmp(name, "system") == 0) {
		vtg->group = VG_SYSTEM;
	} else if (strcmp(name, "top-app") == 0) {
		vtg->group = VG_TOPAPP;
	} else if (strcmp(name, "foreground") == 0) {
		vtg->group = VG_FOREGROUND;
	} else if (strcmp(name, "camera-daemon") == 0) {
		vtg->group = VG_CAMERA;
	} else if (strcmp(name, "background") == 0) {
		vtg->group = VG_BACKGROUND;
	} else if (strcmp(name, "system-background") == 0) {
		vtg->group = VG_SYSTEM_BACKGROUND;
	} else if (strcmp(name, "nnapi-hal") == 0) {
		vtg->group = VG_NNAPI_HAL;
	} else if (strcmp(name, "rt") == 0) {
		vtg->group = VG_RT;
	} else if (strcmp(name, "audio-app") == 0) {
		vtg->group = VG_FOREGROUND;
	} else if (strcmp(name, "dex2oat") == 0) {
		vtg->group = VG_DEX2OAT;
	} else {
		vtg->group = VG_SYSTEM;
	}
}

void vh_sched_setscheduler_uclamp_pixel_mod(void *data, struct task_struct *tsk, int clamp_id,
					    unsigned int value)
{
	trace_sched_setscheduler_uclamp(tsk, clamp_id, value);
	if (trace_clock_set_rate_enabled()) {
		char trace_name[32] = {0};
		scnprintf(trace_name, sizeof(trace_name), "%s_%d",
			clamp_id  == UCLAMP_MIN ? "UCLAMP_MIN" : "UCLAMP_MAX", tsk->pid);
		trace_clock_set_rate(trace_name, value, raw_smp_processor_id());
	}
}

void vh_dup_task_struct_pixel_mod(void *data, struct task_struct *tsk, struct task_struct *orig)
{
	struct vendor_task_struct *v_tsk, *v_orig;
	struct vendor_binder_task_struct *vbinder;

	v_tsk = get_vendor_task_struct(tsk);
	v_orig = get_vendor_task_struct(orig);
	vbinder = get_vendor_binder_task_struct(tsk);
	init_vendor_task_struct(v_tsk);
	v_tsk->group = v_orig->group;
}

void rvh_cpumask_any_and_distribute(void *data, struct task_struct *p,
	const struct cpumask *cpu_valid_mask,
	const struct cpumask *new_mask, int *dest_cpu)
{
	cpumask_t valid_mask;

	if (unlikely(!cpumask_and(&valid_mask, cpu_valid_mask, new_mask)))
		goto out;

	/* find a cpu again for the running/runnable/waking tasks if their
	 * current cpu are not allowed
	 */
	if ((p->on_cpu || p->state == TASK_WAKING || task_on_rq_queued(p)) &&
	    !cpumask_test_cpu(task_cpu(p), new_mask))
		*dest_cpu = find_energy_efficient_cpu(p, task_cpu(p),
						      &valid_mask);

out:
	trace_cpumask_any_and_distribute(p, &valid_mask, *dest_cpu);

	return;
}

void rvh_select_task_rq_fair_pixel_mod(void *data, struct task_struct *p, int prev_cpu, int sd_flag,
				       int wake_flags, int *target_cpu)
{
	int sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);
	bool sync_wakeup = false, prefer_prev = false, sync_boost = false;
	int cpu;

	if (sd_flag == SD_BALANCE_EXEC) {
		*target_cpu = prev_cpu;
		goto out;
	}

	/* sync wake up */
	cpu = smp_processor_id();
	if (sync && cpu_rq(cpu)->nr_running == 1 && cpumask_test_cpu(cpu, p->cpus_ptr) &&
	     cpu_is_in_target_set(p, cpu) && task_fits_capacity(p, cpu, false)) {
		*target_cpu = cpu;
		sync_wakeup = true;
		goto out;
	}

	sync_boost = sync && cpu >= HIGH_CAPACITY_CPU;

	/* prefer prev cpu */
	if (cpumask_test_cpu(prev_cpu, p->cpus_ptr) && cpu_active(prev_cpu) &&
	    cpu_is_idle(prev_cpu) && task_fits_capacity(p, prev_cpu, sync_boost)) {

		struct cpuidle_state *idle_state;
		unsigned int exit_lat = UINT_MAX;

		rcu_read_lock();
		idle_state = idle_get_state(cpu_rq(prev_cpu));

		if (sched_cpu_idle(prev_cpu))
			exit_lat = 0;
		else if (idle_state)
			exit_lat = idle_state->exit_latency;

		rcu_read_unlock();

		if (exit_lat <= C1_EXIT_LATENCY) {
			prefer_prev = true;
			*target_cpu = prev_cpu;
			goto out;
		}
	}

	*target_cpu = find_energy_efficient_cpu(p, prev_cpu, p->cpus_ptr);

out:
	trace_sched_select_task_rq_fair(p, task_util_est(p),
					sync_wakeup, prefer_prev, sync_boost,
					get_vendor_group(p),
					uclamp_eff_value(p, UCLAMP_MIN),
					uclamp_eff_value(p, UCLAMP_MAX),
					prev_cpu, *target_cpu);
}

static struct task_struct *detach_important_task(struct rq *src_rq, int dst_cpu)
{
	struct task_struct *p = NULL, *best_task = NULL, *backup = NULL,
		*backup_ui = NULL, *backup_unfit = NULL;

	lockdep_assert_held(&src_rq->lock);

	rcu_read_lock();

	list_for_each_entry_reverse(p, &src_rq->cfs_tasks, se.group_node) {
		struct vendor_task_struct *vp = get_vendor_task_struct(p);
		bool is_heavy_task = (get_prefer_idle(p) || uclamp_latency_sensitive(p)) && (get_prefer_high_cap(p) || uclamp_boosted(p));
		bool is_important = false;

		if (!cpumask_test_cpu(dst_cpu, p->cpus_ptr))
			continue;

		if (task_running(src_rq, p))
			continue;

		if (!is_heavy_task)
			continue;

	    if ((vp && vp->uclamp_fork_reset && is_heavy_task)
	        || uclamp_eff_value(p, UCLAMP_MIN) > 0)
			is_important = true;

		if (task_fits_capacity(p, dst_cpu, false)) {
			if (!task_fits_capacity(p, src_rq->cpu, false)) {
				// if task is fit for new cpu but not old cpu
				// stop if we found an ADPF important task
				// use it as backup if we found a important task
				if (is_important) {
					best_task = p;
					break;
				}

				backup = p;
			} else {
				if (is_important) {
					backup_ui = p;
					continue;
				}

				if (!backup)
					backup = p;
			}
		} else {
			// if new idle is not capable, use it as backup but not for important task.
			if (!is_important)
				backup_unfit = p;
		}

	}

	if (best_task)
		p = best_task;
	else if (backup_ui)
		p = backup_ui;
	else if (backup)
		p = backup;
	else if (backup_unfit)
		p = backup_unfit;
	else
		p = NULL;

	if (p) {
		/* detach_task */
		deactivate_task(src_rq, p, DEQUEUE_NOCLOCK);
		set_task_cpu(p, dst_cpu);

		if (backup_unfit)
			cpu_rq(dst_cpu)->misfit_task_load = p->se.avg.load_avg;
		else
			cpu_rq(dst_cpu)->misfit_task_load = 0;
	}

	rcu_read_unlock();
	return p;
}

/*
 * In our newidle_balance, We ignore update next_interval, which could lead to
 * the next tick might being triggered prematurely. but that should be fine since
 * this is should not be happening often enough.
 */
void sched_newidle_balance_pixel_mod(void *data, struct rq *this_rq, struct rq_flags *rf,
		int *pulled_task, int *done)
{
	int cpu;
	struct rq *src_rq;
	struct task_struct *p = NULL;
	struct rq_flags src_rf;
	int this_cpu = this_rq->cpu;

	/*
	 * We must set idle_stamp _before_ calling idle_balance(), such that we
	 * measure the duration of idle_balance() as idle time.
	 */
	this_rq->idle_stamp = rq_clock(this_rq);

	/*
	 * Do not pull tasks towards !active CPUs...
	 * Make sure we only pull tasks on near-idle CPUs and not on idle CPUs 
	 */
	if (!cpu_active(this_cpu) || this_cpu >= MID_CAPACITY_CPU || cpu_is_idle(this_cpu))
		return;

	/*
	 * This is OK, because current is on_cpu, which avoids it being picked
	 * for load-balance and preemption/IRQs are still disabled avoiding
	 * further scheduler activity on it and we're being very careful to
	 * re-start the picking loop.
	 */
	rq_unpin_lock(this_rq, rf);
	raw_spin_unlock(&this_rq->lock);

	this_cpu = this_rq->cpu;
	for_each_cpu(cpu, cpu_active_mask) {
		int cpu_importnace = READ_ONCE(cpu_rq(cpu)->uclamp[UCLAMP_MIN].value) +
			READ_ONCE(cpu_rq(cpu)->uclamp[UCLAMP_MAX].value);

		if (cpu == this_cpu)
			continue;

		src_rq = cpu_rq(cpu);
		rq_lock_irqsave(src_rq, &src_rf);
		update_rq_clock(src_rq);

		if (src_rq->active_balance) {
			rq_unlock_irqrestore(src_rq, &src_rf);
			continue;
		}

		if (src_rq->nr_running <= 1) {
			rq_unlock_irqrestore(src_rq, &src_rf);
			continue;
		}

		if (cpu_importnace <= DEFAULT_IMPRATANCE_THRESHOLD || !src_rq->cfs.nr_running) {
			rq_unlock_irqrestore(src_rq, &src_rf);
			continue;
		}

		p = detach_important_task(src_rq, this_cpu);

        if (p) {
		    /* Skip this CPU if the source task cannot migrate */
		    if (!cpumask_test_cpu(this_cpu, p->cpus_ptr))
		        continue;
		}

		rq_unlock_irqrestore(src_rq, &src_rf);

		if (p) {
			attach_one_task(this_rq, p);
			break;
		}
	}

	raw_spin_lock(&this_rq->lock);
	/*
	 * While browsing the domains, we released the rq lock, a task could
	 * have been enqueued in the meantime. Since we're not going idle,
	 * pretend we pulled a task.
	 */
	if (this_rq->cfs.h_nr_running && !*pulled_task)
		*pulled_task = 1;

	/* Is there a task of a high priority class? */
	if (this_rq->nr_running != this_rq->cfs.h_nr_running)
		*pulled_task = -1;

	if (*pulled_task)
		this_rq->idle_stamp = 0;

	if (*pulled_task != 0) {
		*done = 1;
		/* TODO: need implement update_blocked_averages */
	}

	rq_repin_lock(this_rq, rf);

	return;
}
