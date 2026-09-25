#ifndef HOOK_H
#define HOOK_H

int init_hooks(void);
int apply_hook_by_patching(int syscall_id);
void remove_hook_by_patching(int syscall_id);
void restore_all_hooks(void);

#endif /* HOOK_H */
