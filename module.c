#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include "module.h"
#include "hook.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Simone Ercole");
MODULE_DESCRIPTION("System call throttler");
MODULE_VERSION("1.0");


DEFINE_HASHTABLE(syscall_hashtable, BITS_FOR_BUCKETS);
DEFINE_HASHTABLE(prog_hashtable, BITS_FOR_BUCKETS);
DEFINE_HASHTABLE(uid_hashtable, BITS_FOR_BUCKETS);


DEFINE_MUTEX(syscall_hashtable_mutex);
DEFINE_MUTEX(prog_hashtable_mutex);
DEFINE_MUTEX(uid_hashtable_mutex);

int max_syscalls_per_sec = 50000;


static int __init my_module_init(void)
{
    int ret;
    throttle_init();
    
    ret = init_hooks();
    if (ret < 0) {
        printk(KERN_ERR "Impossibile inizializzare gli hook\n");
        return ret;
    }
    
    ret = setup_monitor_device();
    if (ret < 0) {
        printk(KERN_ERR "Impossibile fare il setup del dispositivo\n");
        return ret;
    }

    printk(KERN_INFO "Modulo inserito correttamente!\n");
    return 0;
}

static void cleanup_hashtables(void)
{
    int bkt;
    struct hlist_node *tmp;
    struct target_syscall *ts;
    struct target_prog *tp;
    struct target_uid *tu;
    
    mutex_lock(&syscall_hashtable_mutex);
    hash_for_each_safe(syscall_hashtable, bkt, tmp, ts, node) {
        hash_del(&ts->node); 
        kfree(ts);           
    }
    mutex_unlock(&syscall_hashtable_mutex);


    mutex_lock(&prog_hashtable_mutex);
    hash_for_each_safe(prog_hashtable, bkt, tmp, tp, node) {
        hash_del(&tp->node);
        kfree(tp);
    }
    mutex_unlock(&prog_hashtable_mutex);


    mutex_lock(&uid_hashtable_mutex);
    hash_for_each_safe(uid_hashtable, bkt, tmp, tu, node) {
        hash_del(&tu->node);
        kfree(tu);
    }
    mutex_unlock(&uid_hashtable_mutex);
}

static void __exit my_module_exit(void) {
    cleanup_monitor_device();
    restore_all_hooks();
    throttle_cleanup_timer();
    cleanup_hashtables();
    printk(KERN_INFO "Modulo rimosso!\n");
}

module_init(my_module_init);
module_exit(my_module_exit);
