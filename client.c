#include <stdio.h>                   
#include <stdlib.h>                  
#include <string.h>                  
#include <fcntl.h>                   
#include <unistd.h>                   
#include <errno.h>                   
#include "header/ioctl.h"             
#include "header/syscall_mapping.h"   

#define DEV_PATH "/dev/throttle_dev"

static void usage()
{
    fprintf(stderr,
        "Uso: ./client <comando> [argomento]\n\n"
        "Comandi (richiedono root):\n"
        "  on                         Attiva il monitor\n"
        "  off                        Disattiva il monitor\n"
        "  set_max <N>                Imposta MAX invocazioni/secondo\n"
        "  add_syscall <ID>           Registra una syscall (numero x86-64)\n"
        "  del_syscall <ID>           Deregistra una syscall\n"
        "  add_uid <UID>              Registra uno user id\n"
        "  del_uid <UID>              Deregistra uno user id\n"
        "  add_prog <NOME>            Registra un nome di programma (max 15 char)\n"
        "  del_prog <NOME>            Deregistra un nome di programma\n"
        "  reset_stats                Azzera le statistiche storiche\n"

        "Comandi (chiunque):\n"
        "  stats                      Mostra le statistiche correnti\n"
        "  list_syscalls              Elenca le syscall registrate\n"
        "  list_uids                  Elenca gli UID registrati\n"
        "  list_progs                 Elenca i nomi di programma registrati\n"
        );
}

static int open_dev(void)
{
    int fd = open(DEV_PATH, O_RDWR);
    if (fd < 0) {
        perror(" Impossibile aprire " DEV_PATH);
        exit(EXIT_FAILURE);
    }
    return fd;
}

static long need_int_arg(int argc, char **argv, int idx, const char *name)
{
    char *end;
    long v;
 
    if (idx >= argc) {
        fprintf(stderr, "Manca l'argomento numerico per '%s'\n", name);
        exit(EXIT_FAILURE);
    }
 
    errno = 0;
    v = strtol(argv[idx], &end, 10);
 
    if (*end != '\0') {
        fprintf(stderr, "Argomento non numerico: '%s'\n", argv[idx]);
        exit(EXIT_FAILURE);
    }
    if (errno == ERANGE) {
        fprintf(stderr, "Argomento fuori range: '%s'\n", argv[idx]);
        exit(EXIT_FAILURE);
    }
    return v;
}


static void print_stats(int fd)
{
    struct throttle_stats st;

    if (ioctl(fd, IOCTL_GET_STATS, &st) < 0) {
        perror("IOCTL_GET_STATS fallita");
        exit(EXIT_FAILURE);
    }

    printf("Statistiche throttling:\n");
    printf("  Delay massimo: %.3f ms\n", st.peak_delay_ns / 1e6);
    printf("  Programma (Delay massimo): %.16s\n", st.peak_delay_comm);
    printf("  UID (Delay massimo): %d\n", st.peak_delay_uid);
    printf("  Thread bloccati (avg): %d\n", st.avg_blocked_threads);
    printf("  Thread bloccati (max): %d\n", st.peak_blocked_threads);
}

static void print_list_syscalls(int fd) {
    struct throttle_syscall_list list;
    int i;
     
    if (ioctl(fd, IOCTL_LIST_SYSCALLS, &list) < 0) {
        perror("IOCTL_LIST_SYSCALLS fallita");
        exit(EXIT_FAILURE);
    }
     
    printf("Syscall registrate (%d):\n", list.total_registered);
     
    for (i = 0; i < list.count; i++) {
        int id = list.ids[i];
        const char *name = "Sconosciuta";
         
        name = syscall_sym_names[id];
        printf("  - %d - %s\n", id, name);
    }
     
    if (list.total_registered > list.count) {
        printf("  ... elenco troncato: mostrati %d su %d totali\n",
               list.count, list.total_registered);
    }
}

static void print_list_uids(int fd)
{
    struct throttle_uid_list list;
    int i;

    if (ioctl(fd, IOCTL_LIST_UIDS, &list) < 0) {
        perror("IOCTL_LIST_UIDS fallita");
        exit(EXIT_FAILURE);
    }
    printf("UID registrati (%d):\n", list.total_registered);
    for (i = 0; i < list.count; i++)
        printf("  - %d\n", list.uids[i]);
    if (list.total_registered > list.count)
        printf("  ... elenco troncato: mostrati %d su %d totali\n",
               list.count, list.total_registered);
}

static void print_list_progs(int fd)
{
    struct throttle_prog_list list;
    int i;

    if (ioctl(fd, IOCTL_LIST_PROGS, &list) < 0) {
        perror("[client] IOCTL_LIST_PROGS fallita");
        exit(EXIT_FAILURE);
    }
    printf("Programmi registrati (%d):\n", list.total_registered);
    for (i = 0; i < list.count; i++)
        printf("  - %.16s\n", list.names[i]);
    if (list.total_registered > list.count)
        printf("  ... elenco troncato: mostrati %d su %d totali\n",
               list.count, list.total_registered);
}

int main(int argc, char **argv)
{
    int fd;
    int ival;

    if (argc < 2) {
        usage();
        return EXIT_FAILURE;
    }

    fd = open_dev();

    if (strcmp(argv[1], "on") == 0) {
        if (ioctl(fd, IOCTL_MONITOR_ON) < 0) { perror("IOCTL_MONITOR_ON"); goto err; }
        printf("Monitor attivato.\n");

    } else if (strcmp(argv[1], "off") == 0) {
        if (ioctl(fd, IOCTL_MONITOR_OFF) < 0) { perror("IOCTL_MONITOR_OFF"); goto err; }
        printf("Monitor disattivato.\n");

    } else if (strcmp(argv[1], "set_max") == 0) {
        ival = (int)need_int_arg(argc, argv, 2, "set_max");
        if (ioctl(fd, IOCTL_SET_MAX, &ival) < 0) { perror("IOCTL_SET_MAX"); goto err; }
        printf("Token massimi impostati a %d.\n", ival);

    } else if (strcmp(argv[1], "add_syscall") == 0) {
        ival = (int)need_int_arg(argc, argv, 2, "add_syscall");
        if (ioctl(fd, IOCTL_ADD_SYSCALL, &ival) < 0) { perror("IOCTL_ADD_SYSCALL"); goto err; }
        printf("Syscall %d registrata.\n", ival);

    } else if (strcmp(argv[1], "del_syscall") == 0) {
        ival = (int)need_int_arg(argc, argv, 2, "del_syscall");
        if (ioctl(fd, IOCTL_DEL_SYSCALL, &ival) < 0) { perror("IOCTL_DEL_SYSCALL"); goto err; }
        printf("Syscall %d deregistrata.\n", ival);

    } else if (strcmp(argv[1], "add_uid") == 0) {
        ival = (int)need_int_arg(argc, argv, 2, "add_uid");
        if (ioctl(fd, IOCTL_ADD_UID, &ival) < 0) { perror("IOCTL_ADD_UID"); goto err; }
        printf("UID %d registrato.\n", ival);

    } else if (strcmp(argv[1], "del_uid") == 0) {
        ival = (int)need_int_arg(argc, argv, 2, "del_uid");
        if (ioctl(fd, IOCTL_DEL_UID, &ival) < 0) { perror("IOCTL_DEL_UID"); goto err; }
        printf("UID %d deregistrato.\n", ival);

    } else if (strcmp(argv[1], "add_prog") == 0 || strcmp(argv[1], "del_prog") == 0) {
        struct throttle_prog_info pi;
        unsigned long cmd = (strcmp(argv[1], "add_prog") == 0) ? IOCTL_ADD_PROG_NAME : IOCTL_DEL_PROG_NAME;

        if (argc < 3) { fprintf(stderr, "Manca il nome del programma\n"); goto err; }
        memset(&pi, 0, sizeof(pi));
        strncpy(pi.comm, argv[2], MAX_PROG_NAME - 1);
        pi.comm[MAX_PROG_NAME - 1] = '\0';

        if (ioctl(fd, cmd, &pi) < 0) { perror("IOCTL_*_PROG_NAME"); goto err; }
        printf("Programma '%s' %s.\n", pi.comm,
               cmd == IOCTL_ADD_PROG_NAME ? "registrato" : "deregistrato");

    } else if (strcmp(argv[1], "stats") == 0) {
        print_stats(fd);

    } else if (strcmp(argv[1], "list_syscalls") == 0) {
        print_list_syscalls(fd);

    } else if (strcmp(argv[1], "list_uids") == 0) {
        print_list_uids(fd);

    } else if (strcmp(argv[1], "list_progs") == 0) {
        print_list_progs(fd);
    }

    else if (strcmp(argv[1], "reset_stats") == 0) {
        if (ioctl(fd, IOCTL_RESET_STATS) < 0) { perror("IOCTL_RESET_STATS"); goto err; }
        printf("Statistiche azzerate con successo.\n");

    } 
    else {
        usage();
        close(fd);
        return EXIT_FAILURE;
    }

    close(fd);
    return EXIT_SUCCESS;

err:
    close(fd);
    return EXIT_FAILURE;
}
