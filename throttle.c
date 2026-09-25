#include <linux/kernel.h>
#include <linux/sched.h>   /* Per get_task_comm e current */
#include <linux/cred.h>    /* Per current_euid */
#include <linux/ktime.h>   
#include <linux/string.h>  
#include <linux/jhash.h>    
#include <linux/wait.h>    /* Per wait_queue e sblocco thread */
#include <linux/percpu.h>  
#include <linux/atomic.h>  /* Per atomics e cmpxchg */
#include <linux/hrtimer.h> /* Per hrtimer_setup, hrtimer_start */
#include <linux/math64.h>  /* Per div64_u64 */
#include <linux/smp.h>     /* Per on_each_cpu */
#include <linux/u64_stats_sync.h> /* Per syncp */
#include "module.h" 

struct throttle_stats_percpu {
    u64 peak_delay_ns;
    u64 blocked_sum;        
    u64 blocked_samples;     
    char peak_delay_comm[MAX_PROG_NAME];
    uid_t peak_delay_uid;   
    int peak_blocked_threads;
    struct u64_stats_sync syncp;
};

static DEFINE_PER_CPU(struct throttle_stats_percpu, g_stats_percpu);

static atomic_t g_available_tokens = ATOMIC_INIT(0);     
static atomic64_t g_last_second = ATOMIC64_INIT(0);       
static atomic_t g_blocked_threads_count = ATOMIC_INIT(0);

static DECLARE_WAIT_QUEUE_HEAD(g_throttle_wq);

static struct hrtimer g_wakeup_timer;                  
static atomic_t g_timer_active = ATOMIC_INIT(0);
static atomic_t monitor_active = ATOMIC_INIT(0);


bool is_uid_registered(int uid) {
    struct target_uid *e;
    bool found = false;

    rcu_read_lock();
    hash_for_each_possible_rcu(uid_hashtable, e, node, uid) {
        if (e->uid == uid) {
            found = true;
            break;
        }
    }
    rcu_read_unlock();

    return found;
}

bool is_prog_registered(const char *comm) {
    struct target_prog *e;
    u32 hash_val = jhash(comm, strnlen(comm, MAX_PROG_NAME), 0);
    bool found = false;

    rcu_read_lock();
    hash_for_each_possible_rcu(prog_hashtable, e, node, hash_val) {
        if (strncmp(e->comm, comm, MAX_PROG_NAME) == 0) {
            found = true;
            break;
        }
    }
    rcu_read_unlock();

    return found;
}

static bool try_grab_token(void) {
    return atomic_add_unless(&g_available_tokens, -1, 0);
}

static void maybe_refill_tokens(void) {
    int current_max = READ_ONCE(max_syscalls_per_sec);
    s64 now_sec = (s64)ktime_get_seconds();
    s64 old_sec = atomic64_read(&g_last_second);

    if (old_sec == now_sec) return;

    if (atomic64_cmpxchg(&g_last_second, old_sec, now_sec) == old_sec) {
        atomic_set(&g_available_tokens, current_max);
        if (current_max > 0)
            __wake_up(&g_throttle_wq, TASK_NORMAL, current_max, NULL);
    }
}

static enum hrtimer_restart wakeup_timer_func(struct hrtimer *timer) {
    maybe_refill_tokens();

    if (atomic_read(&monitor_active)) {
        hrtimer_forward_now(timer, ns_to_ktime(NSEC_PER_SEC));
        return HRTIMER_RESTART;
    }

    atomic_set(&g_timer_active, 0);
    return HRTIMER_NORESTART;
}

void throttle_init(void) {
    int cpu;
    hrtimer_setup(&g_wakeup_timer, wakeup_timer_func, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
 
    for_each_possible_cpu(cpu) {
        u64_stats_init(&per_cpu(g_stats_percpu, cpu).syncp);
    }
}

void throttle_cleanup_timer(void) {
    hrtimer_cancel(&g_wakeup_timer);
}

void throttle_set_monitor(bool on) {
    atomic_set(&monitor_active, on ? 1 : 0);
    if (!on) {
        wake_up_all(&g_throttle_wq);
    }
}

static void record_block_start_stats(int current_blocked) {
    unsigned long flags;
    struct throttle_stats_percpu *st;
 
    local_irq_save(flags); 
    st = this_cpu_ptr(&g_stats_percpu);
 
    u64_stats_update_begin(&st->syncp);
    st->blocked_sum += current_blocked;
    st->blocked_samples++;
    if (current_blocked > st->peak_blocked_threads)
        st->peak_blocked_threads = current_blocked;
    u64_stats_update_end(&st->syncp);
 
    local_irq_restore(flags);
}
 
static void record_block_end_stats(u64 delay_ns, uid_t uid, const char *comm) {
    unsigned long flags;
    struct throttle_stats_percpu *st;
 
    local_irq_save(flags);
    st = this_cpu_ptr(&g_stats_percpu);
 
    u64_stats_update_begin(&st->syncp);
    if (delay_ns > st->peak_delay_ns) {
        st->peak_delay_ns = delay_ns;
        st->peak_delay_uid = uid;
        strscpy(st->peak_delay_comm, comm, MAX_PROG_NAME);
    }
    u64_stats_update_end(&st->syncp);
 
    local_irq_restore(flags);
}

long throttle_check(int syscall_id) {
    kuid_t euid;
    uid_t uid_val;
    char comm[MAX_PROG_NAME];
    int current_blocked;
    u64 t0, delay_ns;
    long wait_res;

    if (!atomic_read(&monitor_active))
        return 0;

    euid = current_euid();
    uid_val = __kuid_val(euid);
    get_task_comm(comm, current);
    
    if (!is_prog_registered(comm) && !is_uid_registered(uid_val)) return 0;

    maybe_refill_tokens();

    if (try_grab_token()) return 0;
    
    /* --- INIZIO DEL THROTTLING --- */
    t0 = ktime_get_ns();
    current_blocked = atomic_inc_return(&g_blocked_threads_count);
    record_block_start_stats(current_blocked);

    if (atomic_cmpxchg(&g_timer_active, 0, 1) == 0) {
        unsigned long ns_rem = NSEC_PER_SEC - (ktime_get_ns() % NSEC_PER_SEC);
        hrtimer_start(&g_wakeup_timer, ns_to_ktime(ns_rem), HRTIMER_MODE_REL);
    }

    wait_res = wait_event_interruptible_exclusive(g_throttle_wq,
                    !atomic_read(&monitor_active) || try_grab_token());

    /* --- FINE DEL THROTTLING --- */
    atomic_dec(&g_blocked_threads_count);
    /* Svegliato da un segnale */
    if (wait_res != 0)
        return -ERESTARTSYS;
    
    delay_ns = ktime_get_ns() - t0;
    record_block_end_stats(delay_ns, uid_val, comm);
    return 0;
}

void throttle_get_stats(struct throttle_stats *out) {
    int cpu;
    u64 blocked_sum = 0, blocked_samples = 0;
    int peak_blocked = 0;
    u64 peak_delay_ns = 0;
    char peak_comm[MAX_PROG_NAME] = { 0 };
    uid_t peak_uid = 0;
 
    /* Aggrega i dati di tutte le CPU */
    for_each_possible_cpu(cpu) {
        struct throttle_stats_percpu *st = &per_cpu(g_stats_percpu, cpu);
        unsigned int start;
        u64 t_sum, t_samples, t_peak_delay_ns;
        int t_peak_blocked;
        uid_t t_peak_uid;
        char t_peak_comm[MAX_PROG_NAME];
 
        do {
            start = u64_stats_fetch_begin(&st->syncp);
            t_sum = st->blocked_sum;
            t_samples = st->blocked_samples;
            t_peak_blocked = st->peak_blocked_threads;
            t_peak_delay_ns = st->peak_delay_ns;
            t_peak_uid = st->peak_delay_uid;
            memcpy(t_peak_comm, st->peak_delay_comm, MAX_PROG_NAME);
            
        } while (u64_stats_fetch_retry(&st->syncp, start));
 
        blocked_sum += t_sum;
        blocked_samples += t_samples;
 
        if (t_peak_blocked > peak_blocked)
            peak_blocked = t_peak_blocked;
 
        if (t_peak_delay_ns > peak_delay_ns) {
            peak_delay_ns = t_peak_delay_ns;
            peak_uid = t_peak_uid;
            memcpy(peak_comm, t_peak_comm, MAX_PROG_NAME);
        }
    }
 
    out->peak_delay_ns = peak_delay_ns;
    memcpy(out->peak_delay_comm, peak_comm, MAX_PROG_NAME);
    out->peak_delay_uid = peak_uid;
    out->peak_blocked_threads = peak_blocked;
    out->avg_blocked_threads = blocked_samples ? (int)div64_u64(blocked_sum, blocked_samples) : 0;
}
 
static void reset_stats_this_cpu(void *unused) {
    struct throttle_stats_percpu *st = this_cpu_ptr(&g_stats_percpu);
 
    u64_stats_update_begin(&st->syncp);
    st->blocked_sum = 0;
    st->blocked_samples = 0;
    st->peak_blocked_threads = 0;
    st->peak_delay_ns = 0;
    st->peak_delay_uid = 0;
    memset(st->peak_delay_comm, 0, MAX_PROG_NAME);
    u64_stats_update_end(&st->syncp);
}

void throttle_reset_stats(void) {
    on_each_cpu(reset_stats_this_cpu, NULL, 1);
}
