#include <linux/module.h> /* Per pt_regs, try_module_get, module_put, THIS_MODULE */
#include <linux/kprobes.h>
#include <linux/string.h>
#include <linux/err.h>

#include "hook.h"
#include "module.h"

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
static kallsyms_lookup_name_t my_kallsyms_lookup_name = NULL;
static unsigned long *sys_call_table = NULL;
static void *x64_sys_call_fn = NULL;

typedef void *(*text_poke_bp_t)(void *addr, const void *opcode, size_t len, const void *emulate);
static text_poke_bp_t my_text_poke_bp = NULL;

typedef void (*smp_text_poke_single_t)(void *addr, const void *opcode, size_t len, const void *emulate);
static smp_text_poke_single_t my_smp_text_poke_single = NULL;

typedef int (*kallsyms_lookup_size_offset_t)(unsigned long addr, unsigned long *symbolsize, unsigned long *offset);
static kallsyms_lookup_size_offset_t my_kallsyms_lookup_size_offset = NULL;
static size_t x64_sys_call_size = 0;

struct patch_site {
    uint8_t *call_site;
    uint8_t  rel[5];
};
static struct patch_site patch_sites[MAX_SYSCALLS];

static int resolve_kallsyms(void) {
    int ret;
    struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
    ret = register_kprobe(&kp);
    if (ret < 0) return ret;
    my_kallsyms_lookup_name = (kallsyms_lookup_name_t)kp.addr;
    unregister_kprobe(&kp);
    return 0;
}

static int resolve_symbol(void **target, const char *name) {
    *target = (void *)my_kallsyms_lookup_name(name);
    if (!*target) return -ENOENT;
    return 0;
}

static void get_x64_sys_call_size (void) {
    unsigned long size = 0;
    unsigned long offset = 0;
    my_kallsyms_lookup_size_offset((unsigned long)x64_sys_call_fn, &size, &offset);
    x64_sys_call_size = size;
}

static long hook_func(const struct pt_regs *regs) {
    long orig_ax = (long)regs->orig_ax;
    int syscall_id;
    typedef asmlinkage long (*sys_call_ptr_t)(const struct pt_regs *);
    sys_call_ptr_t original_func;
    long throttle_ret;
    long ret;

    syscall_id = (int)orig_ax;
    
    if (!try_module_get(THIS_MODULE)) {
       return -ERESTARTSYS;
    }

    throttle_ret = throttle_check(syscall_id);
    if (throttle_ret != 0) {
        module_put(THIS_MODULE);
        return throttle_ret;
    }

    original_func = (sys_call_ptr_t)sys_call_table[syscall_id];
    ret = original_func(regs);

    module_put(THIS_MODULE);
    return ret;
}

int apply_hook_by_patching(int syscall_id) {
    uint8_t  *func_ptr = (uint8_t *)x64_sys_call_fn;
    void     *target;
    size_t    i;

    if (patch_sites[syscall_id].call_site != NULL) return -EEXIST;
    target = (void *)sys_call_table[syscall_id];
    if (!target) return -EINVAL;

    for (i = 0; i + 5 <= x64_sys_call_size; ++i) {
        int32_t rel;
        void *call_dest;
        intptr_t new_rel;
        uint8_t opcode = func_ptr[i];

        if (opcode != 0xe8 && opcode != 0xe9) continue;
                 
        rel = *(int32_t *)(func_ptr + i + 1);
        call_dest = (void *)(func_ptr + i + 5 + rel);
        if (call_dest != target) continue;
                  
        new_rel = (intptr_t)hook_func - (intptr_t)(func_ptr + i + 5);
        if (new_rel > S32_MAX || new_rel < S32_MIN) return -ERANGE;
          
        patch_sites[syscall_id].call_site = func_ptr + i;
        memcpy(patch_sites[syscall_id].rel, func_ptr + i , 5);
        
        uint8_t new_instruction[5];
        new_instruction[0] = *(func_ptr + i);
        int32_t int32_rel = (int32_t)new_rel;
        memcpy(&new_instruction[1], &int32_rel, 4);

        if (my_text_poke_bp) my_text_poke_bp(func_ptr + i, new_instruction, 5, NULL);
        else my_smp_text_poke_single(func_ptr + i, new_instruction, 5, NULL);
        
        return 0;
    }
    return -ENOENT;
}

void remove_hook_by_patching(int syscall_id) {
    uint8_t *target_address;
    target_address = (uint8_t *)patch_sites[syscall_id].call_site;
         
    if (my_text_poke_bp) my_text_poke_bp(target_address, patch_sites[syscall_id].rel, 5, NULL);
    else my_smp_text_poke_single(target_address, patch_sites[syscall_id].rel, 5, NULL);
         
    patch_sites[syscall_id].call_site = NULL;
}

int init_hooks(void) {
    int ret;
         
    ret = resolve_kallsyms();
    if (ret) return ret;
    if (resolve_symbol((void **)&sys_call_table, "sys_call_table")) return -ENOENT;
    if (resolve_symbol((void **)&x64_sys_call_fn, "x64_sys_call")) return -ENOENT;
         
    my_text_poke_bp = (text_poke_bp_t)my_kallsyms_lookup_name("text_poke_bp");
    if (!my_text_poke_bp) {
        my_smp_text_poke_single = (smp_text_poke_single_t)my_kallsyms_lookup_name("smp_text_poke_single");
        if (!my_smp_text_poke_single) {
            pr_err("Errore fatale, né text_poke_bp né smp_text_poke_single trovati\n");
            return -ENOENT;
        }
    }
         
    if (resolve_symbol((void **)&my_kallsyms_lookup_size_offset, "kallsyms_lookup_size_offset")) return -ENOENT;
    get_x64_sys_call_size();
    memset(patch_sites, 0, sizeof(patch_sites));
    
    return 0;
}

void restore_all_hooks(void) {
    int i;
    
    for (i = 0; i < MAX_SYSCALLS; i++) {
        if (patch_sites[i].call_site != NULL)
            remove_hook_by_patching(i);
    }
    throttle_set_monitor(false);
}
