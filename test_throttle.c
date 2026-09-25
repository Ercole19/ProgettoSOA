#define _GNU_SOURCE         
#include <stdio.h>           
#include <stdlib.h>          
#include <string.h>          
#include <unistd.h>       
#include <pthread.h>         
#include <time.h>            
#include <errno.h>           
#include <fcntl.h>           
#include <sys/syscall.h>     
#include "header/ioctl.h"    

#define DEV_PATH "/dev/throttle_dev"
#define SYS_TARGET 39
volatile int g_stop = 0;
char g_self_comm[16];

static void read_self_comm(void) {
    FILE *f = fopen("/proc/self/comm", "r");
    if (f) {
        if (fgets(g_self_comm, sizeof(g_self_comm), f))
            g_self_comm[strcspn(g_self_comm, "\n")] = '\0';
        fclose(f);
    } else {
        strcpy(g_self_comm, "test_throttle");
    }
}

static double ts_diff_ms(struct timespec *t0, struct timespec *t1) {
    return (t1->tv_sec - t0->tv_sec) * 1000.0 + (t1->tv_nsec - t0->tv_nsec) / 1e6;
}

static int require_root(const char *mode) {
    if (geteuid() != 0) {
        fprintf(stderr, "%s richiede EUID 0.\n", mode);
        return 0;
    }
    return 1;
}

/* Performance Test */
struct perf_arg {
    int iters;
    long calls;
    double sum_ms;
    double min_ms;
    double max_ms;
    long throttled_calls;
    double threshold_ms; 
};

void* perf_worker(void* arg) {
    struct perf_arg *a = arg;
    a->sum_ms = 0;
    a->min_ms = 1e9; 
    a->max_ms = 0;
    a->throttled_calls = 0;

    for (int i = 0; i < a->iters; i++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        syscall(SYS_TARGET);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = ts_diff_ms(&t0, &t1);
        a->calls = a->calls +1;
        a->sum_ms += ms;
        if (ms < a->min_ms) a->min_ms = ms;
        if (ms > a->max_ms) a->max_ms = ms;
        if (ms > a->threshold_ms) a->throttled_calls++;
        usleep(100);
        
    }
    return NULL;
}

void print_perf_stats(const char* title, double total_time_ms, struct perf_arg* args, int n_threads, int show_throttle) {
    double sum_ms = 0, min_ms = 1e9, max_ms = 0;
    long throttled = 0;
    long total_calls = 0;
    
    for (int i = 0; i < n_threads; i++) {
        sum_ms += args[i].sum_ms;
        total_calls += args[i].calls;
        if (args[i].min_ms < min_ms) min_ms = args[i].min_ms;
        if (args[i].max_ms > max_ms) max_ms = args[i].max_ms;
        throttled += args[i].throttled_calls;
    }
    
    double mean_ms = sum_ms / total_calls;
    double rate = total_calls / (total_time_ms / 1000.0);

    printf("\n--- %s ---\n", title);
    printf("  Chiamate totali : %ld\n", total_calls);
    printf("  Rate globale    : %.2f syscall/sec\n", rate);
    printf("  Delay Minimo    : %.6f ms \n", min_ms);
    printf("  Delay Massimo   : %.6f ms \n", max_ms);
    printf("  Delay Medio     : %.6f ms \n", mean_ms);
    
    if (show_throttle) {
        printf("  Chiamate >%.3fms : %ld (%.1f%%)\n", args[0].threshold_ms, throttled, (total_calls > 0) ? (100.0 * throttled / total_calls) : 0);
    }
}

int run_performancetest(int fd, int n_threads, int iters) {
    if (!require_root("performancetest")) return 1;
    
    pthread_t *tids = malloc(n_threads * sizeof(pthread_t));
    struct perf_arg *args = calloc(n_threads, sizeof(struct perf_arg));
    struct timespec t0, t1;
    int sys_id = SYS_TARGET;  
    int target_uid = 0; 
    
    printf("== Performance Test: %d thread, %d iterazioni per thread ==\n", n_threads, iters);

    /*Scenario senza hook*/
    ioctl(fd, IOCTL_MONITOR_OFF);
    ioctl(fd, IOCTL_DEL_SYSCALL, &sys_id);
    
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < n_threads; i++) {
        args[i].iters = iters;
        pthread_create(&tids[i], NULL, perf_worker, &args[i]);
    }
    for (int i = 0; i < n_threads; i++) pthread_join(tids[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    
    double time_unmonitored = ts_diff_ms(&t0, &t1);
    print_perf_stats("SCENARIO 1: SENZA HOOK (Baseline)", time_unmonitored, args, n_threads, 0);

    double baseline_max_ms = 0;
    for (int i = 0; i < n_threads; i++) {
        if (args[i].max_ms > baseline_max_ms) 
            baseline_max_ms = args[i].max_ms;
    }

    double dynamic_threshold = baseline_max_ms + 1.5;
    printf("\n[*] Soglia dinamica calcolata per il throttling: > %.3f ms\n", dynamic_threshold);
    
    /*Scenario con hook*/
    ioctl(fd, IOCTL_MONITOR_OFF);
    ioctl(fd, IOCTL_DEL_SYSCALL, &sys_id);
    ioctl(fd, IOCTL_DEL_PROG_NAME, g_self_comm);
    ioctl(fd, IOCTL_DEL_UID, &target_uid);
    usleep(10000); 
    
    int limit_low = 100; 
    ioctl(fd, IOCTL_ADD_SYSCALL, &sys_id);
    ioctl(fd, IOCTL_ADD_PROG_NAME, g_self_comm);
    ioctl(fd, IOCTL_ADD_UID, &target_uid);
    ioctl(fd, IOCTL_SET_MAX, &limit_low);
    ioctl(fd, IOCTL_MONITOR_ON);

    memset(args, 0, n_threads * sizeof(struct perf_arg));
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < n_threads; i++) {
        args[i].iters = iters;
        args[i].threshold_ms = dynamic_threshold;
        pthread_create(&tids[i], NULL, perf_worker, &args[i]);
    }
    for (int i = 0; i < n_threads; i++) pthread_join(tids[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double time_throttled = ts_diff_ms(&t0, &t1);
    print_perf_stats("SCENARIO 2: CON HOOK E THROTTLING", time_throttled, args, n_threads, 1);
    
    ioctl(fd, IOCTL_MONITOR_OFF);
    ioctl(fd, IOCTL_DEL_SYSCALL, &sys_id);
    ioctl(fd, IOCTL_DEL_PROG_NAME, g_self_comm);
    ioctl(fd, IOCTL_DEL_UID, &target_uid);
    int new_limit = 5000;
    ioctl(fd, IOCTL_SET_MAX, &new_limit);

    free(tids);
    free(args);
    return 0;
}

/* Reconfig Test */
void* reconf_worker(void* arg) {
    (void)arg;
    while (!g_stop) syscall(SYS_TARGET);
    return NULL;
}

void* reconf_churner(void* arg) {
    int fd = *(int*)arg;
    int uid = 0, max = 100; 
    while (!g_stop) {
        ioctl(fd, IOCTL_ADD_PROG_NAME, g_self_comm);
        ioctl(fd, IOCTL_DEL_PROG_NAME, g_self_comm);
        ioctl(fd, IOCTL_ADD_UID, &uid);
        ioctl(fd, IOCTL_DEL_UID, &uid);
        max = (max == 100) ? 50 : 100;
        ioctl(fd, IOCTL_SET_MAX, &max);
        usleep(100);
    }
    return NULL;
}

int run_reconfigtest(int fd, int duration_sec, int n_threads) {
    if (!require_root("reconfigtest")) return 1;
    pthread_t *w_tids = malloc(n_threads * sizeof(pthread_t));
    pthread_t c_tid;
    int id = SYS_TARGET, max = 1000;
    printf("== Reconfigtest (%d sec, %d thread): stress RCU/Mutex ==\n", duration_sec, n_threads);
    
    ioctl(fd, IOCTL_ADD_SYSCALL, &id);
    ioctl(fd, IOCTL_SET_MAX, &max);
    ioctl(fd, IOCTL_MONITOR_ON);
    
    for (int i = 0; i < n_threads; i++) pthread_create(&w_tids[i], NULL, reconf_worker, NULL);
    pthread_create(&c_tid, NULL, reconf_churner, &fd);
    
    sleep(duration_sec);
    g_stop = 1;
    pthread_join(c_tid, NULL);
    
    for (int i = 0; i < n_threads; i++) pthread_join(w_tids[i], NULL);
    
    ioctl(fd, IOCTL_MONITOR_OFF);
    ioctl(fd, IOCTL_DEL_SYSCALL, &id);
    max = 5000;
    ioctl(fd, IOCTL_SET_MAX, &max);
    
    free(w_tids);
    printf("PASSATO! Nessun kernel panic o deadlock rilevato.\n");
    return 0;
}

/* Monitor Test */
void* mon_worker(void* arg) {
    (void)arg;
    printf("Chiamo e mi metto in attesa... \n");
    syscall(SYS_TARGET);
    printf("Sbloccato! \n");
    return NULL;
}

int run_monitortest(int fd, int wait_sec) {
    if (!require_root("monitortest")) return 1;

    pthread_t tids[15]; 
    int id = SYS_TARGET, max = 1;
    struct timespec t0, t1;

    printf("== Monitortest (attesa %d sec prima dello spegnimento) ==\n", wait_sec);
    
    ioctl(fd, IOCTL_ADD_SYSCALL, &id);
    ioctl(fd, IOCTL_ADD_PROG_NAME, g_self_comm);
    ioctl(fd, IOCTL_SET_MAX, &max);
    ioctl(fd, IOCTL_MONITOR_ON);

    syscall(SYS_TARGET);
    
    for (int i = 0; i < 15; i++) {
        pthread_create(&tids[i], NULL, mon_worker, NULL);
    }
    
    sleep(wait_sec);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    ioctl(fd, IOCTL_MONITOR_OFF);
    
    for (int i = 0; i < 15; i++) {
        pthread_join(tids[i], NULL);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &t1);

    ioctl(fd, IOCTL_DEL_PROG_NAME, g_self_comm);
    ioctl(fd, IOCTL_DEL_SYSCALL, &id);
    max = 5000;
    ioctl(fd, IOCTL_SET_MAX, &max);

    double ms = ts_diff_ms(&t0, &t1);
    printf("Tempo di sblocco: %.2f ms\n", ms);

    if (ms <= 10.0) {
        printf("PASSATO! Wake-up immediato confermato.\n");
        return 0;
    }

    printf("FALLITO! Sblocco tardivo.\n");
    return 1;
}

void* base_worker(void* arg) {
    (void)arg;
    syscall(SYS_TARGET);
    return NULL;
}

/* PermTest */
int run_permtest(int fd) {
    if (!require_root("permtest")) return 1;
    
    int val = 50;
    int id = SYS_TARGET;
    int fails = 0;
    struct throttle_stats st;
    struct throttle_syscall_list list;

    printf("== Permtest: controllo permessi ==\n");

    if (seteuid(65534) != 0) { 
        perror("seteuid(nobody) fallita"); 
        return 1; 
    }

    /* Comandi di SCRITTURA / CONFIGURAZIONE: devono essere rifiutati con EPERM */
    errno = 0;
    if (ioctl(fd, IOCTL_SET_MAX, &val) == 0 || errno != EPERM) { 
        puts("FALLITO: SET_MAX (doveva fallire con EPERM)"); 
        fails++; 
    }

    errno = 0;
    if (ioctl(fd, IOCTL_MONITOR_ON) == 0 || errno != EPERM) { 
        puts("FALLITO: MONITOR_ON (doveva fallire con EPERM)"); 
        fails++; 
    }

    errno = 0;
    if (ioctl(fd, IOCTL_ADD_SYSCALL, &id) == 0 || errno != EPERM) { 
        puts("FALLITO: ADD_SYSCALL (doveva fallire con EPERM)"); 
        fails++; 
    }

    /*Comandi di SOLA LETTURA: devono funzionare anche per utenti non root */
    errno = 0;
    if (ioctl(fd, IOCTL_GET_STATS, &st) != 0) { 
        puts("FALLITO: GET_STATS doveva funzionare per utenti non-root"); 
        fails++; 
    }

    errno = 0;
    if (ioctl(fd, IOCTL_LIST_SYSCALLS, &list) != 0) { 
        puts("FALLITO: LIST_SYSCALLS doveva funzionare per utenti non-root"); 
        fails++; 
    }

    /* Ripristina i privilegi di root */
    if (seteuid(0) != 0) { 
        perror("seteuid(0) fallita"); 
        return 1; 
    }

    if (fails > 0) {
        printf("FALLITO! Rilevati %d errori di controllo permessi.\n", fails);
        return 1;
    }

    printf("PASSATO! Controlli permessi verificati (scritture bloccate con EPERM, letture permesse).\n");
    return 0;
}

/* BoundTest */
int run_boundtest(int fd) {
    if (!require_root("boundtest")) return 1;

    int fails = 0;
    int invalid_id_neg = -1;
    int invalid_id_huge = 999999;
    int invalid_rate_neg = -10;
    int invalid_uid_neg = -1;
    
    struct throttle_prog_info pi_empty = {0};
    struct throttle_prog_info pi_overflow;

    printf("== Boundtest: controllo input non validi (Syscall, Rate, UID, Prog) ==\n");
    errno = 0;
    if (ioctl(fd, IOCTL_ADD_SYSCALL, &invalid_id_neg) == 0 || errno != EINVAL) {
        fprintf(stderr, "FALLITO: ADD_SYSCALL (-1) doveva dare EINVAL (errno=%d: %s)\n",
                errno, strerror(errno));
        fails++;
    }
    errno = 0;
    if (ioctl(fd, IOCTL_ADD_SYSCALL, &invalid_id_huge) == 0 || errno != EINVAL) {
        fprintf(stderr, "FALLITO: ADD_SYSCALL (999999) doveva dare EINVAL (errno=%d: %s)\n",
                errno, strerror(errno));
        fails++;
    }
    errno = 0;
    if (ioctl(fd, IOCTL_SET_MAX, &invalid_rate_neg) == 0 || errno != EINVAL) {
        fprintf(stderr, "FALLITO: SET_MAX (-10) doveva dare EINVAL (errno=%d: %s)\n",
                errno, strerror(errno));
        fails++;
    }
    errno = 0;
    if (ioctl(fd, IOCTL_ADD_UID, &invalid_uid_neg) == 0 || errno != EINVAL) {
        fprintf(stderr, "FALLITO: ADD_UID (-1) doveva dare EINVAL (errno=%d: %s)\n",
                errno, strerror(errno));
        fails++;
    }
    pi_empty.comm[0] = '\0';
    errno = 0;
    if (ioctl(fd, IOCTL_ADD_PROG_NAME, &pi_empty) == 0 || errno != EINVAL) {
        fprintf(stderr, "FALLITO: ADD_PROG_NAME (stringa vuota) doveva dare EINVAL (errno=%d: %s)\n",
                errno, strerror(errno));
        fails++;
    }
    memset(pi_overflow.comm, 'A', sizeof(pi_overflow.comm));
    errno = 0;
    if (ioctl(fd, IOCTL_ADD_PROG_NAME, &pi_overflow) == 0 || errno != EINVAL) {
        fprintf(stderr, "FALLITO: ADD_PROG_NAME (non null-terminated) doveva dare EINVAL (errno=%d: %s)\n",
                errno, strerror(errno));
        fails++;
    }

    if (fails > 0) {
        printf("FALLITO! %d controlli di limite non superati.\n", fails);
        return 1;
    }
    printf("PASSATO! Rifiuto di tutti gli input non validi confermato.\n");
    return 0;
}


/* Unload Test*/
void* blocking_worker(void* arg) {
    (void)arg;
    struct timespec req = { .tv_sec = 4, .tv_nsec = 0 }; 
    syscall(35, &req, NULL); 
    return NULL;
}

int run_unloadtest(int fd) {
    if (!require_root("unloadtest")) return 1;

    printf("== Unloadtest: Verifica protezione Reference Counting ==\n");
    int id = SYS_TARGET, max = 1;
    pthread_t tids[10];

    printf("\n--- FASE 1: Thread bloccati nella wait queue del modulo ---\n");
    ioctl(fd, IOCTL_ADD_SYSCALL, &id);
    ioctl(fd, IOCTL_ADD_PROG_NAME, g_self_comm);
    ioctl(fd, IOCTL_SET_MAX, &max);
    ioctl(fd, IOCTL_MONITOR_ON);

    syscall(SYS_TARGET);

    for (int i = 0; i < 10; i++) {
        pthread_create(&tids[i], NULL, mon_worker, NULL);
    }
    usleep(100000);

    printf("Tento l'unload con thread in coda di throttling (dovrebbe fallire)...\n");
    int ret_fail = system("rmmod throttle_module 2>/dev/null");
    
    if (ret_fail == 0) {
        printf("FALLIMENTO: rmmod ha avuto successo! Il reference count non funziona!\n");
        return 1;
    }
    printf("SUCCESSO: rmmod è stato respinto in sicurezza dal kernel (-EBUSY).\n");

    ioctl(fd, IOCTL_MONITOR_OFF);
    for (int i = 0; i < 10; i++) {
        pthread_join(tids[i], NULL);
    }
    ioctl(fd, IOCTL_DEL_SYSCALL, &id);

    printf("\n--- FASE 2: Thread bloccati nell'handler nativo (SYS_nanosleep) ---\n");
    int nanosleep_id = 35;
    int high_max = 10000;

    ioctl(fd, IOCTL_ADD_SYSCALL, &nanosleep_id);
    ioctl(fd, IOCTL_SET_MAX, &high_max);
    ioctl(fd, IOCTL_MONITOR_ON);

    for (int i = 0; i < 3; i++) {
        pthread_create(&tids[i], NULL, blocking_worker, NULL);
    }
    usleep(500000);

    printf("Tento l'unload con thread in sleep profondo nel kernel (dovrebbe fallire)...\n");
    ret_fail = system("rmmod throttle_module 2>/dev/null");
    
    if (ret_fail == 0) {
        printf("FALLIMENTO: rmmod ha avuto successo mentre i thread erano in nanosleep!\n");
        return 1;
    }
    printf("SUCCESSO: rmmod respinto (-EBUSY). L'hook protegge anche i blocchi nativi.\n");

    printf("Attesa uscita naturale dalla nanosleep (circa 4 secondi)...\n");
    for (int i = 0; i < 3; i++) {
        pthread_join(tids[i], NULL);
    }
    
    ioctl(fd, IOCTL_MONITOR_OFF);
    ioctl(fd, IOCTL_DEL_SYSCALL, &nanosleep_id);
    ioctl(fd, IOCTL_DEL_PROG_NAME, g_self_comm);

    printf("\n--- FASE 3: Smontaggio a modulo vuoto ---\n");
    close(fd);
    sleep(1);
    
    int ret_success = system("rmmod throttle_module");
    if (ret_success != 0) {
        printf("FALLIMENTO: rmmod ha restituito un errore anche a modulo vuoto.\n");
        return 1;
    }

    if (access(DEV_PATH, F_OK) == 0) {
        printf("FALLIMENTO: /dev/throttle_dev esiste ancora!\n");
        return 1;
    }

    printf("PASSATO! Protezione del reference counting certificata in tutti gli scenari.\n");
    
    printf("Ricarico il modulo per mantenere il sistema coerente...\n");
    system("insmod throttle_module.ko");
    usleep(100000);
    return 0;
}


static void print_help(const char *prog_name) {
    printf("Uso: %s [comando] [argomenti...]\n\n", prog_name);
    printf("Comandi disponibili:\n");
    printf("  performancetest [threads] [iters]  Esegue il test di performance (default: 10 100)\n");
    printf("  reconfigtest [sec] [threads]       Stress test su RCU e Mutex (default: 5 10)\n");
    printf("  monitortest [sec]                  Verifica il risveglio dei thread allo spegnimento (default: 5)\n");
    printf("  permtest                           Verifica i controlli sui permessi associati ai comandi\n");
    printf("  boundtest                          Verifica la gestione di input non validi\n");
    printf("  unloadtest                         Stress test sullo smontaggio del modulo \n");
    printf("  help, -h, --help                   Mostra questo messaggio di aiuto\n");
}

int main(int argc, char **argv) {
    read_self_comm();
    const char *mode = (argc > 1) ? argv[1] : "performancetest";

    if (strcmp(mode, "help") == 0 || strcmp(mode, "-h") == 0 || strcmp(mode, "--help") == 0) {
        print_help(argv[0]);
        return 0;
    }

    int fd = open(DEV_PATH, O_RDWR);
    if (fd < 0) { perror("Errore apertura device"); return 1; }

    int ret = 0;
    if (strcmp(mode, "performancetest") == 0) {
        int th = (argc > 2) ? atoi(argv[2]) : 10;
        int it = (argc > 3) ? atoi(argv[3]) : 100;
        ret = run_performancetest(fd, th, it);
    }
    else if (strcmp(mode, "reconfigtest") == 0) {
        int dur = (argc > 2) ? atoi(argv[2]) : 5;
        int th  = (argc > 3) ? atoi(argv[3]) : 10;
        ret = run_reconfigtest(fd, dur, th);
    }
    else if (strcmp(mode, "monitortest") == 0) {
        int wait = (argc > 2) ? atoi(argv[2]) : 5;
        ret = run_monitortest(fd, wait);
    }
    else if (strcmp(mode, "permtest") == 0) { ret = run_permtest(fd);    }
    else if (strcmp(mode, "boundtest") == 0) { ret = run_boundtest(fd);  } 
    else if (strcmp(mode, "unloadtest") == 0) 	 { ret = run_unloadtest(fd); }
    else { 
        printf("Modalità sconosciuta: %s\n\n", mode); 
        print_help(argv[0]);
        ret = 1; 
    }

    close(fd);
    return ret;
}