#include <linux/kernel.h>   /* Per printk */
#include <linux/uaccess.h>  /* Per copy_from_user / copy_to_user */
#include <linux/slab.h>     
#include <linux/jhash.h>    
#include <linux/rcupdate.h>
#include <linux/string.h>   
#include <linux/cred.h>     /* Per current_euid() */
#include <linux/uidgid.h>   /* Per uid_eq e GLOBAL_ROOT_UID */

#include "ioctl.h"
#include "module.h"
#include "hook.h"

static bool cmd_requires_root(unsigned int cmd) {
    switch (cmd) {
    case IOCTL_GET_STATS:
    case IOCTL_LIST_SYSCALLS:
    case IOCTL_LIST_UIDS:
    case IOCTL_LIST_PROGS:
        return false; 
    default:
        return true; 
    }
}

long manage_ioctl_function(struct file *file, unsigned int cmd, unsigned long arg) {
    int temp_val;
    struct throttle_prog_info prog_info;

    if (cmd_requires_root(cmd) && !uid_eq(current_euid(), GLOBAL_ROOT_UID)) {
        return -EPERM;
    }

    switch (cmd) {
    case IOCTL_MONITOR_ON:
        throttle_set_monitor(true);
        printk(KERN_INFO "Monitor ATTIVATO.\n");
        break;

    case IOCTL_MONITOR_OFF:
        throttle_set_monitor(false);
        printk(KERN_INFO "Monitor DISATTIVATO.\n");
        break;

    case IOCTL_SET_MAX:
        if (copy_from_user(&temp_val, (int __user *)arg, sizeof(int))) return -EFAULT;
        if (temp_val < 0) return -EINVAL;

        WRITE_ONCE(max_syscalls_per_sec, temp_val);
        printk(KERN_INFO "Nuovo limite MAX ricevuto: %d\n", temp_val);
        break;

    case IOCTL_ADD_SYSCALL: {
        struct target_syscall *new_syscall, *existing_syscall;
        int ret;

        if (copy_from_user(&temp_val, (int __user *)arg, sizeof(int))) return -EFAULT;

        if (temp_val < 0 || temp_val >= MAX_SYSCALLS) return -EINVAL;
        if (!syscall_sym_names[temp_val]) return -EINVAL;

        new_syscall = kmalloc(sizeof(*new_syscall), GFP_KERNEL);
        if (!new_syscall) return -ENOMEM;
        new_syscall->syscall_id = temp_val;

        mutex_lock(&syscall_hashtable_mutex);

        /* Controlla che non sia già stata inserita */
        hash_for_each_possible(syscall_hashtable, existing_syscall, node, temp_val) {
            if (existing_syscall->syscall_id == temp_val) {
                mutex_unlock(&syscall_hashtable_mutex);
                kfree(new_syscall);
                return -EEXIST;
            }
        }

        hash_add_rcu(syscall_hashtable, &new_syscall->node, temp_val);

        ret = apply_hook_by_patching(temp_val);
        if (ret) {
            hash_del_rcu(&new_syscall->node);
            mutex_unlock(&syscall_hashtable_mutex);
            kfree_rcu(new_syscall, rcu);
            return ret;
        }

        mutex_unlock(&syscall_hashtable_mutex);
        printk(KERN_INFO "Syscall %d registrata.\n", temp_val);
        break;
    }

    /* --- RIMOZIONE SYSCALL --- */
    case IOCTL_DEL_SYSCALL: {
        struct target_syscall *entry = NULL;
        struct hlist_node *tmp;
        bool found = false;

        if (copy_from_user(&temp_val, (int __user *)arg, sizeof(int))) return -EFAULT;
        if (temp_val < 0 || temp_val >= MAX_SYSCALLS) return -EINVAL;

        mutex_lock(&syscall_hashtable_mutex);

        hash_for_each_possible_safe(syscall_hashtable, entry, tmp, node, temp_val) {
            if (entry->syscall_id == temp_val) {
                hash_del_rcu(&entry->node);
                found = true;
                break;
            }
        }
        if (!found) {
            mutex_unlock(&syscall_hashtable_mutex);
            return -ENOENT;
        }

        remove_hook_by_patching(temp_val);

        mutex_unlock(&syscall_hashtable_mutex);

        kfree_rcu(entry, rcu);
        printk(KERN_INFO "Syscall %d deregistrata.\n", temp_val);
        break;
    }

    /* --- INSERIMENTO NOME PROCESSO --- */
    case IOCTL_ADD_PROG_NAME: {
        struct target_prog *new_prog, *existing_prog;
        u32 hash_val;

        if (copy_from_user(&prog_info, (struct throttle_prog_info __user *)arg, sizeof(prog_info)))
            return -EFAULT;

        if (strnlen(prog_info.comm, MAX_PROG_NAME) == MAX_PROG_NAME)
            return -EINVAL;

        if (prog_info.comm[0] == '\0')
            return -EINVAL;

        hash_val = jhash(prog_info.comm, strlen(prog_info.comm), 0);

        new_prog = kmalloc(sizeof(*new_prog), GFP_KERNEL);
        if (!new_prog)
            return -ENOMEM;
        strscpy(new_prog->comm, prog_info.comm, MAX_PROG_NAME);

        mutex_lock(&prog_hashtable_mutex);
        hash_for_each_possible(prog_hashtable, existing_prog, node, hash_val) {
            if (strncmp(existing_prog->comm, prog_info.comm, MAX_PROG_NAME) == 0) {
                mutex_unlock(&prog_hashtable_mutex);
                kfree(new_prog);
                return -EEXIST;
            }
        }

        hash_add_rcu(prog_hashtable, &new_prog->node, hash_val);
        mutex_unlock(&prog_hashtable_mutex);
        printk(KERN_INFO "Programma %s registrato.\n", prog_info.comm);
        break;
    }

    /* --- INSERIMENTO UID --- */
    case IOCTL_ADD_UID: {
        struct target_uid *new_uid, *existing_uid;

        if (copy_from_user(&temp_val, (int __user *)arg, sizeof(int))) return -EFAULT;
        if (temp_val < 0) return -EINVAL;

        new_uid = kmalloc(sizeof(*new_uid), GFP_KERNEL);
        if (!new_uid) return -ENOMEM;
        new_uid->uid = temp_val;

        mutex_lock(&uid_hashtable_mutex);
        hash_for_each_possible(uid_hashtable, existing_uid, node, temp_val) {
            if (existing_uid->uid == temp_val) {
                mutex_unlock(&uid_hashtable_mutex);
                kfree(new_uid);
                return -EEXIST;
            }
        }

        hash_add_rcu(uid_hashtable, &new_uid->node, temp_val);
        mutex_unlock(&uid_hashtable_mutex);
        printk(KERN_INFO "UID %d registrato.\n", temp_val);
        break;
    }

    /* --- RIMOZIONE UID --- */
    case IOCTL_DEL_UID: {
        struct target_uid *entry;
        struct hlist_node *tmp;
        bool found = false;

        if (copy_from_user(&temp_val, (int __user *)arg, sizeof(int))) return -EFAULT;
        if (temp_val < 0) return -EINVAL;

        mutex_lock(&uid_hashtable_mutex);
        hash_for_each_possible_safe(uid_hashtable, entry, tmp, node, temp_val) {
            if (entry->uid == temp_val) {
                hash_del_rcu(&entry->node);
                found = true;
                break;
            }
        }
        mutex_unlock(&uid_hashtable_mutex);

        if (found) {
            kfree_rcu(entry, rcu);
            printk(KERN_INFO "UID %d deregistrato.\n", temp_val);
        } else {
            return -ENOENT;
        }
        break;
    }

    /* --- RIMOZIONE NOME PROCESSO --- */
    case IOCTL_DEL_PROG_NAME: {
        struct target_prog *entry;
        struct hlist_node *tmp;
        u32 hash_val;
        bool found = false;

        if (copy_from_user(&prog_info, (struct throttle_prog_info __user *)arg, sizeof(prog_info))) return -EFAULT;
        prog_info.comm[MAX_PROG_NAME - 1] = '\0';
        hash_val = jhash(prog_info.comm, strnlen(prog_info.comm, MAX_PROG_NAME), 0);

        mutex_lock(&prog_hashtable_mutex);
        hash_for_each_possible_safe(prog_hashtable, entry, tmp, node, hash_val) {
            if (strncmp(entry->comm, prog_info.comm, MAX_PROG_NAME) == 0) {
                hash_del_rcu(&entry->node);
                found = true;
                break;
            }
        }
        mutex_unlock(&prog_hashtable_mutex);

        if (found) {
            kfree_rcu(entry, rcu);
            printk(KERN_INFO "Programma %s deregistrato.\n", prog_info.comm);
        } else {
            return -ENOENT;
        }
        break;
    }

    /* --- COMANDI STATISTICHE --- */
    case IOCTL_GET_STATS: {
        struct throttle_stats stats = {0}; 
        throttle_get_stats(&stats); 

        if (copy_to_user((struct throttle_stats __user *)arg, &stats, sizeof(stats)))
            return -EFAULT;
        break;
    }

    case IOCTL_RESET_STATS:
        throttle_reset_stats();
        break;

    /* --- COMANDI LETTURA LISTE  --- */
    case IOCTL_LIST_SYSCALLS: {
        struct throttle_syscall_list *list;
        struct target_syscall *e;
        int bkt;

        list = kzalloc(sizeof(*list), GFP_KERNEL);
        if (!list) return -ENOMEM;

        rcu_read_lock();
        hash_for_each_rcu(syscall_hashtable, bkt, e, node) {
            list->total_registered++;
            if (list->count < MAX_LIST_ITEMS)
                list->ids[list->count++] = e->syscall_id;
        }
        rcu_read_unlock();

        if (copy_to_user((struct throttle_syscall_list __user *)arg, list, sizeof(*list))) {
            kfree(list);
            return -EFAULT;
        }
        kfree(list);
        break;
    }

    case IOCTL_LIST_UIDS: {
        struct throttle_uid_list *list;
        struct target_uid *e;
        int bkt;

        list = kzalloc(sizeof(*list), GFP_KERNEL);
        if (!list) return -ENOMEM;

        rcu_read_lock();
        hash_for_each_rcu(uid_hashtable, bkt, e, node) {
            list->total_registered++;
            if (list->count < MAX_LIST_ITEMS)
                list->uids[list->count++] = e->uid;
        }
        rcu_read_unlock();

        if (copy_to_user((struct throttle_uid_list __user *)arg, list, sizeof(*list))) {
            kfree(list);
            return -EFAULT;
        }
        kfree(list);
        break;
    }

    case IOCTL_LIST_PROGS: {
        struct throttle_prog_list *list;
        struct target_prog *e;
        int bkt;

        list = kzalloc(sizeof(*list), GFP_KERNEL);
        if (!list) return -ENOMEM;

        rcu_read_lock();
        hash_for_each_rcu(prog_hashtable, bkt, e, node) {
            list->total_registered++;
            if (list->count < MAX_LIST_ITEMS) {
                strscpy(list->names[list->count], e->comm, MAX_PROG_NAME);
                list->count++;
            }
        }
        rcu_read_unlock();

        if (copy_to_user((struct throttle_prog_list __user *)arg, list, sizeof(*list))) {
            kfree(list);
            return -EFAULT;
        }
        kfree(list);
        break;
    }

    default:
        return -ENOTTY;
    }

    return 0;
}

