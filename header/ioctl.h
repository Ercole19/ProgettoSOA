#ifndef IOCTL_H
#define IOCTL_H

#ifdef __KERNEL__
  #include <linux/ioctl.h>
  struct file;
#else
  #include <sys/ioctl.h>
#endif

#define MAX_PROG_NAME 16
#define MAX_LIST_ITEMS 64
#define MAGIC_N 'T'         

struct throttle_prog_info {
    char comm[MAX_PROG_NAME];
};

struct throttle_stats {
    unsigned long long peak_delay_ns;
    char peak_delay_comm[MAX_PROG_NAME];
    int peak_delay_uid;
    int avg_blocked_threads;
    int peak_blocked_threads;
};

struct throttle_syscall_list {
    int count;             
    int total_registered; 
    int ids[MAX_LIST_ITEMS];
};

struct throttle_uid_list {
    int count;
    int total_registered;
    int uids[MAX_LIST_ITEMS];
};

struct throttle_prog_list {
    int count;
    int total_registered;
    char names[MAX_LIST_ITEMS][MAX_PROG_NAME];
};

#define IOCTL_MONITOR_ON      _IO(MAGIC_N, 0)
#define IOCTL_MONITOR_OFF     _IO(MAGIC_N, 1)
#define IOCTL_SET_MAX         _IOW(MAGIC_N, 2, int)
#define IOCTL_ADD_SYSCALL     _IOW(MAGIC_N, 3, int)
#define IOCTL_DEL_SYSCALL     _IOW(MAGIC_N, 4, int)
#define IOCTL_ADD_UID         _IOW(MAGIC_N, 5, int)
#define IOCTL_DEL_UID         _IOW(MAGIC_N, 6, int)
#define IOCTL_ADD_PROG_NAME   _IOW(MAGIC_N, 7, struct throttle_prog_info)
#define IOCTL_DEL_PROG_NAME   _IOW(MAGIC_N, 8, struct throttle_prog_info)
#define IOCTL_GET_STATS       _IOR(MAGIC_N, 9, struct throttle_stats)
#define IOCTL_LIST_SYSCALLS   _IOR(MAGIC_N, 10, struct throttle_syscall_list)
#define IOCTL_LIST_UIDS       _IOR(MAGIC_N, 11, struct throttle_uid_list)
#define IOCTL_LIST_PROGS      _IOR(MAGIC_N, 12, struct throttle_prog_list)
#define IOCTL_RESET_STATS     _IO(MAGIC_N, 13)

#ifdef __KERNEL__
long manage_ioctl_function(struct file *file, unsigned int cmd, unsigned long arg);
#endif

#endif /* IOCTL_H */
