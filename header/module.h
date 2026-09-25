#ifndef MODULE_H
#define MODULE_H

#include <linux/hashtable.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include "syscall_mapping.h"
#include "ioctl.h"
#define BITS_FOR_BUCKETS    8

struct target_syscall {
    int                 syscall_id;
    struct rcu_head     rcu;     
    struct hlist_node   node; 
};

struct target_prog {
    char                comm[MAX_PROG_NAME];
    struct rcu_head     rcu;
    struct hlist_node   node;
};

struct target_uid {
    int                 uid;
    struct rcu_head     rcu;
    struct hlist_node   node;
};

extern DECLARE_HASHTABLE(syscall_hashtable, BITS_FOR_BUCKETS);
extern DECLARE_HASHTABLE(prog_hashtable, BITS_FOR_BUCKETS);
extern DECLARE_HASHTABLE(uid_hashtable, BITS_FOR_BUCKETS);


extern struct mutex syscall_hashtable_mutex;
extern struct mutex prog_hashtable_mutex;
extern struct mutex uid_hashtable_mutex;

extern int max_syscalls_per_sec;

bool is_uid_registered(int uid);
bool is_prog_registered(const char *comm);

int  setup_monitor_device(void);
void cleanup_monitor_device(void);

void throttle_init(void);
long throttle_check(int syscall_id);
void throttle_set_monitor(bool on);
void throttle_cleanup_timer(void);
void throttle_get_stats(struct throttle_stats *out);
void throttle_reset_stats(void);

#endif /* MODULE_H */
