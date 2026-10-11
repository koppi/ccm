/*
 * ccm - Cluster CPU monitor
 *
 * Copyright (c) 2026 Jakob Flierl <jakob.flierl@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#define _GNU_SOURCE /* pipe2() */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <ncurses.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define VERSION "0.0.9"
#define DEFAULT_HOSTS_FILE "hosts.txt"
#define HOME_HOSTS_FILE ".ccmrc"
#define FALLBACK_HOSTS_FILE "/etc/ccm.conf"

#define HOSTS_FILE "hosts.txt"
#define MAX_HOSTS 10000
#define MAX_HOSTNAME 256
#define HOSTNAME_COL_WIDTH 16
#define STATS_COL_WIDTH 48 /* " Load%  usr% sys% nice% idle%  Mem%  Temp   U" */
#define STATS_COL_WIDTH_COMPACT 28 /* " Ld%  usr sys idle  Mem% Tmp U" */

/* How a host is queried. A "talos:" prefix on a host selects talosctl. */
#define PROTO_SSH 0
#define PROTO_TALOS 1
#define TALOS_PREFIX "talos:"
#define TALOS_TIMEOUT_MS 3000 /* per talosctl call; unreachable nodes stall */
#define TALOS_FIRST_SAMPLE_MS 300 /* gap between the two first CPU samples */

/* Host information structure */
typedef struct {
  char hostname[MAX_HOSTNAME];
  int proto;                 /* PROTO_SSH or PROTO_TALOS */
  double cpu_usage;          /* Total CPU usage percentage */
  double user_pct, nice_pct; /* User and nice time percentages */
  double system_pct, idle_pct,
      iowait_pct; /* System, idle, and I/O wait percentages */
  unsigned long long prev_user, prev_nice,
      prev_system; /* Previous CPU counters */
  unsigned long long prev_idle, prev_iowait,
      prev_total;  /* Previous idle and total */
  int initialized; /* Flag if previous values are initialized */
  int ssh_failed;    /* Flag if SSH/talosctl connection failed */
  double mem_usage;  /* Memory utilization percentage */
  int mem_initialized; /* Flag if memory values are initialized */
  unsigned long long mem_total, mem_used, mem_shared, mem_buffers, mem_cached;
  double cpu_temp;
  int temp_initialized;
  int temp_probed;       /* Talos: sensor search done (path may be empty) */
  char temp_path[96];    /* Talos: sysfs file holding the CPU temperature */
  double gpu_usage;      /* GPU busy percentage (amdgpu) */
  int gpu_initialized;   /* Flag if the host reported a GPU load */
  int gpu_probed;        /* Talos: GPU search done (path may be empty) */
  char gpu_path[96];     /* Talos: sysfs file holding the GPU busy percentage */
  pthread_mutex_t lock; /* Per-host mutex for concurrent access */
} HostInfo;

/* Forward declarations */
int get_host_info(const char *hostname, HostInfo *host);

/* Global array of hosts and host count */
HostInfo hosts[MAX_HOSTS];
int host_count = 0;
int running = 1;
double update_interval = 0.5;
int num_threads = 4;

/* Add a host; a leading "talos:" selects talosctl, anything else uses SSH */
void add_host(const char *spec) {
  int proto = PROTO_SSH;

  if (host_count >= MAX_HOSTS)
    return;

  if (strncmp(spec, TALOS_PREFIX, strlen(TALOS_PREFIX)) == 0) {
    proto = PROTO_TALOS;
    spec += strlen(TALOS_PREFIX);
    while (*spec == ' ')
      spec++;
  }
  if (*spec == '\0')
    return;

  memset(&hosts[host_count], 0, sizeof(hosts[host_count]));
  snprintf(hosts[host_count].hostname, MAX_HOSTNAME, "%s", spec);
  hosts[host_count].proto = proto;
  pthread_mutex_init(&hosts[host_count].lock, NULL);
  host_count++;
}

/* Read hosts from a file, one hostname per line */
int read_hosts(const char *filename) {
  FILE *fp = fopen(filename, "r");
  if (!fp)
    return -1;

  char line[MAX_HOSTNAME];

  while (fgets(line, sizeof(line), fp) && host_count < MAX_HOSTS) {
    line[strcspn(line, "\n")] = 0;
    if (strlen(line) > 0 && line[0] != '#')
      add_host(line);
  }

  fclose(fp);
  return host_count;
}

/* Parse comma-separated list of hostnames */
void parse_hosts_list(const char *hostlist) {
  char *list = strdup(hostlist);
  char *token = strtok(list, ",");

  while (token != NULL && host_count < MAX_HOSTS) {
    while (*token == ' ')
      token++;
    char *end = token + strlen(token) - 1;
    while (end > token && *end == ' ')
      end--;
    *(end + 1) = '\0';

    if (strlen(token) > 0)
      add_host(token);

    token = strtok(NULL, ",");
  }

  free(list);
}

/* Worker thread arguments */
typedef struct {
  int start_idx;
  int step;
} ThreadArg;

/* Worker thread to update CPU data for a subset of hosts */
void *worker_thread_func(void *arg) {
  ThreadArg *targ = (ThreadArg *)arg;
  for (int i = targ->start_idx; i < host_count; i += targ->step) {
    pthread_mutex_lock(&hosts[i].lock);
    get_host_info(hosts[i].hostname, &hosts[i]);
    pthread_mutex_unlock(&hosts[i].lock);
  }
  return NULL;
}

/* Background thread function to update CPU data in parallel */
void *update_thread_func(void *arg) {
  (void)arg; /* Suppress unused parameter warning */

  while (running) {
    pthread_t threads[100];
    ThreadArg thread_args[100];
    int threads_to_use = (host_count < num_threads) ? host_count : num_threads;

    /* Spawn worker threads */
    for (int t = 0; t < threads_to_use; t++) {
      thread_args[t].start_idx = t;
      thread_args[t].step = threads_to_use;
      pthread_create(&threads[t], NULL, worker_thread_func, &thread_args[t]);
    }

    /* Wait for all threads to complete */
    for (int t = 0; t < threads_to_use; t++) {
      pthread_join(threads[t], NULL);
    }

    /* Sleep in small increments to check running flag */
    for (int s = 0; s < 10 && running; s++) {
      napms((int)(update_interval * 100));
    }
  }
  return NULL;
}

/* Query a host over SSH (Linux or FreeBSD) */
static int get_host_info_ssh(const char *hostname, HostInfo *host) {
  char cmd[1024];
  FILE *fp;
  char line[256];

  /* Combine all queries into one command */
  snprintf(cmd, sizeof(cmd),
           "ssh -x -T -o ConnectTimeout=1 -o BatchMode=yes -o StrictHostKeyChecking=no %s "
           "\"uname -s; "
           "if [ \\$(uname -s) = 'FreeBSD' ]; then "
           "  top -b -n 1 | head -n 5; "
           "  (sysctl -n dev.cpu.0.temperature 2>/dev/null || sysctl -n hw.acpi.thermal.tz0.temperature 2>/dev/null || sysctl -n dev.acpi_ibm.0.thermal 2>/dev/null) | head -n 1 | awk '{print \\$1}' | sed 's/C//'; "
           "else "
           "  top -bn 1 | head -n 5; "
           "  for g in /sys/class/drm/card[0-9]*/device/gpu_busy_percent; do "
           "    [ -r \\\"\\$g\\\" ] && { echo \\\"GPU \\$(cat \\$g)\\\"; break; }; "
           "  done 2>/dev/null; "
           "  for d in /sys/class/thermal/thermal_zone*; do "
           "    case \\$(cat \\$d/type 2>/dev/null) in "
           "      cpu-thermal|x86_pkg_temp|cpu|acpitz) cat \\$d/temp 2>/dev/null && exit 0;; "
           "    esac; "
           "  done; "
           "  for f in /sys/devices/platform/coretemp.*/hwmon/hwmon*/temp*_input /sys/class/hwmon/hwmon*/temp*_input; do "
           "    [ -f \\\"\\$f\\\" ] && { cat \\\"\\$f\\\" 2>/dev/null && exit 0; }; "
           "  done 2>/dev/null; "
           "  cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 'N/A'; "
           "fi\"",
           hostname);

  fp = popen(cmd, "r");
  if (!fp) { host->ssh_failed = 1; return -1; }

  /* 1. Get OS */
  if (!fgets(line, sizeof(line), fp)) { pclose(fp); host->ssh_failed = 1; return -1; }
  line[strcspn(line, "\n")] = 0;
  int is_freebsd = (strcmp(line, "FreeBSD") == 0);

  if (is_freebsd) {
    /* 2. FreeBSD: Parse top output */
    if (!fgets(line, sizeof(line), fp) || !fgets(line, sizeof(line), fp)) {} /* Skip header */
    
    double u, n, s, i;
    if (fgets(line, sizeof(line), fp) && 
        sscanf(line, "CPU: %lf%% user, %lf%% nice, %lf%% system, %*f%% interrupt, %lf%% idle", 
               &u, &n, &s, &i) >= 4) {
      host->user_pct = u; host->nice_pct = n; host->system_pct = s; host->idle_pct = i;
      host->cpu_usage = 100.0 - i;
    }
    
    double a, ia, w, f;
    if (fgets(line, sizeof(line), fp) &&
        sscanf(line, "Mem: %lfM Active, %lfM Inact, %lfM Wired, %lfM Free", 
               &a, &ia, &w, &f) >= 4) {
      host->mem_total = (unsigned long long)(a + ia + w + f);
      host->mem_used = (unsigned long long)(a + ia + w);
      host->mem_buffers = 0; /* Buffers not explicitly reported on FreeBSD top */
      host->mem_usage = (host->mem_total > 0) ? ((double)host->mem_used / host->mem_total) * 100.0 : 0.0;
      host->mem_initialized = 1;
    }
    
    /* 3. FreeBSD: Parse temperature - read remaining lines to find value */
    char temp_line[256];
    host->temp_initialized = 0;
    while (fgets(temp_line, sizeof(temp_line), fp)) {
      char *ptr = strchr(temp_line, 'C');
      if (ptr) *ptr = '\0';
      double temp = atof(temp_line);
      if (temp > 0.0) {
        host->cpu_temp = temp;
        host->temp_initialized = 1;
        break;
      }
    }
  } else {
    /* 2. Linux: Parse top output */
    if (!fgets(line, sizeof(line), fp) || !fgets(line, sizeof(line), fp)) {} /* Skip header */
    double u, s, n, i;
    if (fgets(line, sizeof(line), fp) &&
        sscanf(line, "%%Cpu(s): %lf us, %lf sy, %lf ni, %lf id", &u, &s, &n, &i) >= 4) {
      host->user_pct = u; host->system_pct = s; host->nice_pct = n; host->idle_pct = i;
      host->cpu_usage = 100.0 - i;
    }
    double total, free, used, buff;
    if (fgets(line, sizeof(line), fp) &&
        sscanf(line, "MiB Mem : %lf total, %lf free, %lf used, %lf buff", &total, &free, &used, &buff) >= 4) {
      host->mem_total = (unsigned long long)total;
      host->mem_used = (unsigned long long)used;
      host->mem_buffers = (unsigned long long)buff;
      host->mem_usage = (total > 0) ? (used / total) * 100.0 : 0.0;
      host->mem_initialized = 1;
    }
    
    /* 3. Linux: Parse GPU load and temperature - read remaining lines. The
     * optional "GPU <percent>" line comes before the temperature. */
    char temp_line[256];
    double gpu = 0.0;
    int gpu_found = 0;
    host->temp_initialized = 0;
    while (fgets(temp_line, sizeof(temp_line), fp)) {
      if (strncmp(temp_line, "GPU ", 4) == 0) {
        if (temp_line[4] >= '0' && temp_line[4] <= '9') {
          gpu = atof(temp_line + 4);
          gpu_found = 1;
        }
      } else if (strncmp(temp_line, "N/A", 3) != 0 && strlen(temp_line) > 1) {
        host->cpu_temp = atof(temp_line) / 1000.0;
        host->temp_initialized = (host->cpu_temp > 0);
        if (host->temp_initialized) break;
      }
    }
    host->gpu_usage = gpu;
    host->gpu_initialized = gpu_found;
  }
  
  pclose(fp);
  host->initialized = 1; host->ssh_failed = 0;
  return 0;
}

/* Run argv without a shell and capture its stdout into buf (NUL-terminated,
 * truncated to bufsz - 1 bytes). Returns 0 if the command exited with status 0
 * within timeout_ms, -1 otherwise. */
static int run_capture(char *const argv[], char *buf, size_t bufsz,
                       int timeout_ms) {
  int fds[2], status = 0, eof = 0;
  size_t len = 0;
  pid_t pid;
  posix_spawn_file_actions_t fa;
  struct timespec start, now;

  buf[0] = '\0';
  /* O_CLOEXEC: other worker threads spawn children concurrently and must not
   * inherit the write end, or we would never see EOF. */
  if (pipe2(fds, O_CLOEXEC) < 0)
    return -1;

  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  close(fds[1]);
  if (rc != 0) {
    close(fds[0]);
    return -1;
  }

  clock_gettime(CLOCK_MONOTONIC, &start);
  while (1) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    int left = timeout_ms - (int)((now.tv_sec - start.tv_sec) * 1000 +
                                  (now.tv_nsec - start.tv_nsec) / 1000000);
    struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
    int n = (left > 0) ? poll(&pfd, 1, left) : 0;
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break; /* timeout or poll error */

    char scratch[1024];
    ssize_t r;
    if (len < bufsz - 1)
      r = read(fds[0], buf + len, bufsz - 1 - len);
    else
      r = read(fds[0], scratch, sizeof(scratch)); /* drain the rest */
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0) {
      eof = (r == 0);
      break;
    }
    if (len < bufsz - 1)
      len += (size_t)r;
  }
  buf[len] = '\0';
  close(fds[0]);

  if (!eof)
    kill(pid, SIGKILL);
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
  return (eof && WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* Read a file on a Talos node with `talosctl read` */
static int talos_read(const char *node, const char *path, char *buf,
                      size_t bufsz) {
  char nodes_arg[MAX_HOSTNAME + 16];
  /* --nodes=VALUE keeps a hostname starting with '-' from acting as a flag */
  snprintf(nodes_arg, sizeof(nodes_arg), "--nodes=%s", node);
  char *argv[] = {"talosctl", nodes_arg, "read", (char *)path, NULL};
  return run_capture(argv, buf, bufsz, TALOS_TIMEOUT_MS);
}

/* Cumulative CPU time counters from the aggregate "cpu" line of /proc/stat */
typedef struct {
  unsigned long long user, nice, system, idle, iowait, total;
} CpuSample;

static int talos_cpu_sample(const char *node, CpuSample *cs) {
  char buf[16384];
  unsigned long long irq = 0, softirq = 0, steal = 0;

  cs->iowait = 0;
  if (talos_read(node, "/proc/stat", buf, sizeof(buf)) != 0)
    return -1;
  if (sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &cs->user,
             &cs->nice, &cs->system, &cs->idle, &cs->iowait, &irq, &softirq,
             &steal) < 4)
    return -1;
  /* guest time is already part of user/nice, so it is not added again */
  cs->total = cs->user + cs->nice + cs->system + cs->idle + cs->iowait + irq +
              softirq + steal;
  return 0;
}

/* Value in kB of a /proc/meminfo field such as "MemTotal:", 0 if absent */
static unsigned long long meminfo_kb(const char *text, const char *key) {
  size_t klen = strlen(key);
  for (const char *p = text; p && *p; ) {
    if (strncmp(p, key, klen) == 0)
      return strtoull(p + klen, NULL, 10);
    p = strchr(p, '\n');
    if (p)
      p++;
  }
  return 0;
}

/* Look for the CPU temperature sensor in sysfs: thermal zones first, then
 * hwmon, matching the same sensors the SSH path looks for. Stores the path of
 * the file holding the temperature in millidegrees, or "" if none is found. */
static void talos_find_temp(const char *node, char *path, size_t pathsz) {
  static const struct {
    const char *prefix, *id_file, *temp_file, *names[5];
  } sensors[] = {
      {"/sys/class/thermal/thermal_zone", "type", "temp",
       {"cpu-thermal", "x86_pkg_temp", "cpu", "acpitz", NULL}},
      {"/sys/class/hwmon/hwmon", "name", "temp1_input",
       {"coretemp", "k10temp", "zenpower", "cpu_thermal", NULL}},
  };

  path[0] = '\0';
  for (size_t s = 0; s < sizeof(sensors) / sizeof(sensors[0]); s++) {
    /* Indexes are contiguous; the first missing one ends the search */
    for (int i = 0; i < 32; i++) {
      char file[128], id[64];
      snprintf(file, sizeof(file), "%s%d/%s", sensors[s].prefix, i,
               sensors[s].id_file);
      if (talos_read(node, file, id, sizeof(id)) != 0)
        break;
      id[strcspn(id, "\n")] = '\0';
      for (int k = 0; sensors[s].names[k]; k++) {
        if (strcmp(id, sensors[s].names[k]) == 0) {
          snprintf(path, pathsz, "%s%d/%s", sensors[s].prefix, i,
                   sensors[s].temp_file);
          return;
        }
      }
    }
  }
}

/* Find the DRM card that exposes amdgpu's gpu_busy_percent. The card index
 * differs between machines (a firmware framebuffer can take card0), so try the
 * first few. Stores the sysfs path, or "" if the node has no such GPU. */
static void talos_find_gpu(const char *node, char *path, size_t pathsz) {
  path[0] = '\0';
  for (int i = 0; i < 4; i++) {
    char file[96], buf[32];
    snprintf(file, sizeof(file), "/sys/class/drm/card%d/device/gpu_busy_percent",
             i);
    if (talos_read(node, file, buf, sizeof(buf)) == 0) {
      snprintf(path, pathsz, "%s", file);
      return;
    }
  }
}

/* Query a Talos node with talosctl. Talos has no shell or SSH, so the raw
 * counters in /proc and sysfs are read through the machine API and CPU usage is
 * derived from the difference between two samples. Percentages follow top(1):
 * idle excludes iowait, and system excludes irq/softirq. */
static int get_host_info_talos(const char *node, HostInfo *host) {
  char buf[16384];
  CpuSample cur;

  if (talos_cpu_sample(node, &cur) != 0) {
    host->prev_total = 0; /* don't average CPU across the outage */
    host->ssh_failed = 1;
    return -1;
  }

  /* No previous sample yet: take a second one shortly after for a delta */
  if (host->prev_total == 0) {
    struct timespec gap = {0, TALOS_FIRST_SAMPLE_MS * 1000000L};
    host->prev_user = cur.user;
    host->prev_nice = cur.nice;
    host->prev_system = cur.system;
    host->prev_idle = cur.idle;
    host->prev_iowait = cur.iowait;
    host->prev_total = cur.total;
    nanosleep(&gap, NULL);
    if (talos_cpu_sample(node, &cur) != 0) {
      host->ssh_failed = 1;
      return -1;
    }
  }

  /* Counters going backwards means the node rebooted: start over */
  if (cur.total <= host->prev_total || cur.user < host->prev_user ||
      cur.nice < host->prev_nice || cur.system < host->prev_system ||
      cur.idle < host->prev_idle || cur.iowait < host->prev_iowait) {
    host->prev_total = 0;
  } else {
    double dt = (double)(cur.total - host->prev_total);
    host->user_pct = (cur.user - host->prev_user) / dt * 100.0;
    host->nice_pct = (cur.nice - host->prev_nice) / dt * 100.0;
    host->system_pct = (cur.system - host->prev_system) / dt * 100.0;
    host->idle_pct = (cur.idle - host->prev_idle) / dt * 100.0;
    host->iowait_pct = (cur.iowait - host->prev_iowait) / dt * 100.0;
    host->cpu_usage = 100.0 - host->idle_pct;
    host->prev_user = cur.user;
    host->prev_nice = cur.nice;
    host->prev_system = cur.system;
    host->prev_idle = cur.idle;
    host->prev_iowait = cur.iowait;
    host->prev_total = cur.total;
    host->initialized = 1;
  }

  if (talos_read(node, "/proc/meminfo", buf, sizeof(buf)) == 0) {
    unsigned long long total = meminfo_kb(buf, "MemTotal:");
    unsigned long long free = meminfo_kb(buf, "MemFree:");
    /* Same as top's "buff/cache" */
    unsigned long long cache = meminfo_kb(buf, "Buffers:") +
                               meminfo_kb(buf, "Cached:") +
                               meminfo_kb(buf, "SReclaimable:");
    unsigned long long used = (total > free + cache) ? total - free - cache : 0;
    if (total > 0) {
      host->mem_total = total / 1024; /* MiB, like top */
      host->mem_used = used / 1024;
      host->mem_buffers = cache / 1024;
      host->mem_usage = (double)used / total * 100.0;
      host->mem_initialized = 1;
    }
  }

  /* Find the temperature sensor once; later passes read the cached path */
  if (!host->temp_probed) {
    talos_find_temp(node, host->temp_path, sizeof(host->temp_path));
    host->temp_probed = 1;
  }
  /* The display reads these without waiting for the lock, so only assign once
   * the (slow) read is done rather than clearing the flag up front */
  double temp = 0.0;
  if (host->temp_path[0] &&
      talos_read(node, host->temp_path, buf, sizeof(buf)) == 0)
    temp = atof(buf) / 1000.0;
  if (temp > 0.0)
    host->cpu_temp = temp;
  host->temp_initialized = (temp > 0.0);

  /* GPU load: same approach, find the card once and then read its path */
  if (!host->gpu_probed) {
    talos_find_gpu(node, host->gpu_path, sizeof(host->gpu_path));
    host->gpu_probed = 1;
  }
  double gpu = 0.0;
  int gpu_found = 0;
  if (host->gpu_path[0] &&
      talos_read(node, host->gpu_path, buf, sizeof(buf)) == 0 &&
      buf[0] >= '0' && buf[0] <= '9') {
    gpu = atof(buf);
    gpu_found = 1;
  }
  host->gpu_usage = gpu;
  host->gpu_initialized = gpu_found;

  host->ssh_failed = 0;
  return 0;
}

/* Query a host using the transport it was configured with */
int get_host_info(const char *hostname, HostInfo *host) {
  if (host->proto == PROTO_TALOS)
    return get_host_info_talos(hostname, host);
  return get_host_info_ssh(hostname, host);
}

/* Print help message */
void print_help(const char *prog) {
  printf("Usage: %s [OPTIONS]\n", prog);
  printf("Monitor CPU, GPU and memory usage across multiple hosts via SSH, or via\n"
         "talosctl for Talos Linux nodes (prefix the host with \"talos:\").\n\n");
  printf("Options:\n");
  printf("  -n, --interval SEC  Update interval in seconds (default: 0.5)\n");
  printf("  --hostfile FILE  Use specified hosts file (default: %s, fallback: "
         "~/%s, then %s)\n",
         DEFAULT_HOSTS_FILE, HOME_HOSTS_FILE, FALLBACK_HOSTS_FILE);
  printf("  -H, --hosts LIST Comma-separated list of hosts to monitor\n");
  printf("  -t, --threads N  Number of parallel SSH/talosctl threads (default: 4)\n");
  printf("  -h, --help       Show this help message\n");
  printf("  -v, --version    Show version information\n\n");
  printf("Copyright (c) 2026 Jakob Flierl <jakob.flierl@gmail.com>\n");
}

/* Print version information */
void print_version() { printf("ccm version %s\n", VERSION); }

/* Update the ncurses display with current CPU and memory usage for all hosts */
int update_display() {
  static const char *spinners[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
  static int frame = 0;
  const char *spinner = spinners[frame % 10];
  frame++;

  clear();

  int max_y, max_x;
  getmaxyx(stdscr, max_y, max_x);

  int min_height = 3;
  int stats_width = STATS_COL_WIDTH_COMPACT;
  int min_width = HOSTNAME_COL_WIDTH + 2 + stats_width;
  if (max_y < min_height || max_x < min_width) {
    return 1;
  }

  int cpu_bar_start = HOSTNAME_COL_WIDTH + 2;
  int show_bars = 0;
  int bar_width = 0;
  int mem_bar_start = cpu_bar_start;
  int gpu_bar_start = cpu_bar_start;
  int show_gpu = 0;
  int pct_col = cpu_bar_start;
  int compact_mode = 0;

  /* Calculate actual stats width needed for wide mode */
  /* Format: "%5.1f %5.1f %5.1f %5.1f  %02d  %s" = 5+1+5+1+5+1+5+2+2+2+3 = 32 */
  int wide_stats_width = 32;

  /* Calculate actual stats width needed for compact mode */
  /* Format: "%3.0f %3.0f %3.0f %02d %s" = 3+1+3+1+3+1+2+1+3 = 18 */
  int compact_stats_width = 18;

  /* Show bars only if terminal is wide enough for bars + full stats */
  int total_bar_space = max_x - cpu_bar_start - wide_stats_width - 6; /* -6 for "[ ]  [ ]" + margin */
  if (total_bar_space >= 24) { /* Need at least 2*10 + 4 for two minimal bars */
    show_bars = 1;
    compact_mode = 0;

    /* Make bars fill available space */
    bar_width = total_bar_space / 2 - 2;
    if (bar_width < 10)
      bar_width = 10;

    /* Add a GPU bar if any host reports a GPU and three bars of at least 8
     * still fit (90 columns); a third bar costs two brackets and a two column
     * gap. Clusters without GPUs keep the two bar layout. */
    int any_gpu = 0;
    for (int i = 0; i < host_count && !any_gpu; i++)
      any_gpu = hosts[i].gpu_initialized;
    if (any_gpu && (total_bar_space - 4) / 3 - 2 >= 8) {
      show_gpu = 1;
      bar_width = (total_bar_space - 4) / 3 - 2;
    }

    stats_width = wide_stats_width;
    mem_bar_start = cpu_bar_start + bar_width + 4;
    gpu_bar_start = mem_bar_start + bar_width + 4;
    pct_col = max_x - stats_width; /* Right-align stats */
  } else {
    compact_mode = 1;
    stats_width = compact_stats_width;
    pct_col = max_x - stats_width; /* Right-align stats */
  }

  int row = 1;

  /* Print header row */
  char header[max_x + 1];
  int hpos = 0;
  hpos += snprintf(header + hpos, sizeof(header) - hpos, " %-*s", HOSTNAME_COL_WIDTH, "Hostname");
  if (show_bars) {
    hpos += snprintf(header + hpos, sizeof(header) - hpos, " [");
    hpos += snprintf(header + hpos, sizeof(header) - hpos, "%-*s", bar_width, "CPU load");
    hpos += snprintf(header + hpos, sizeof(header) - hpos, "]  [");
    hpos += snprintf(header + hpos, sizeof(header) - hpos, "%-*s", bar_width, "Mem usage");
    hpos += snprintf(header + hpos, sizeof(header) - hpos, "]");
    if (show_gpu) {
      hpos += snprintf(header + hpos, sizeof(header) - hpos, "  [");
      hpos += snprintf(header + hpos, sizeof(header) - hpos, "%-*s", bar_width, "GPU load");
      hpos += snprintf(header + hpos, sizeof(header) - hpos, "]");
    }
  }
  int mid_width = pct_col - hpos;
  if (mid_width > 0) {
    hpos += snprintf(header + hpos, sizeof(header) - hpos, "%*s", mid_width, "");
  }
  if (compact_mode) {
    hpos += snprintf(header + hpos, sizeof(header) - hpos, "%3s %3s %3s  %4s %c", "usr", "sys", "idl",
              " °C", ' ');
  } else {
    hpos += snprintf(header + hpos, sizeof(header) - hpos, "%5s %5s %5s %5s %5s   %c", "usr%", "sys%",
              "nice%", "idle%", "CPU°C", ' ');
  }
  if (hpos > max_x)
    header[max_x] = '\0';
  while ((int)strlen(header) < max_x)
    strcat(header, " ");
  attron(COLOR_PAIR(5));
  mvprintw(row, 0, "%s", header);
  attroff(COLOR_PAIR(5));

  /* Print each host's CPU and memory usage */
  for (int i = 0; i < host_count && (i + 3) < max_y; i++) {
    row = i + 2;
    char display_name[HOSTNAME_COL_WIDTH + 1];
    strncpy(display_name, hosts[i].hostname, HOSTNAME_COL_WIDTH);
    display_name[HOSTNAME_COL_WIDTH] = '\0';
    mvprintw(row, 0, "%-*s", HOSTNAME_COL_WIDTH, display_name);

    int locked = (pthread_mutex_trylock(&hosts[i].lock) == 0);

    if (hosts[i].ssh_failed) {
      if (show_bars) {
        attron(COLOR_PAIR(4));
        mvprintw(row, cpu_bar_start, "[");
        mvprintw(row, cpu_bar_start + 1 + bar_width, "]");
        mvprintw(row, mem_bar_start, "[");
        mvprintw(row, mem_bar_start + 1 + bar_width, "]");
        if (show_gpu) {
          mvprintw(row, gpu_bar_start, "[");
          mvprintw(row, gpu_bar_start + 1 + bar_width, "]");
        }
        attroff(COLOR_PAIR(4));
      }

      attron(COLOR_PAIR(7));
      if (compact_mode) {
        mvprintw(row, pct_col, "%4s %4s %4s  %2s %s", "--.-", "--.-", "--.-",
                 "--", locked ? " " : spinner);
      } else {
        mvprintw(row, pct_col, "%5s %5s %5s %5s %5s %s", "--.-", "--.-", "--.-",
                 "--.-", "--.-", locked ? " " : spinner);
      }
      attroff(COLOR_PAIR(7));
    } else if (hosts[i].initialized) {
      if (show_bars) {
        /* Draw CPU bar */
        int cpu_bars = (int)(hosts[i].cpu_usage / 100.0 * bar_width);
        if (cpu_bars > bar_width)
          cpu_bars = bar_width;

        int usr_bars = (int)(hosts[i].user_pct / 100.0 * bar_width);
        int sys_bars = (int)(hosts[i].system_pct / 100.0 * bar_width);
        int nice_bars = (int)(hosts[i].nice_pct / 100.0 * bar_width);

        attron(COLOR_PAIR(4));
        mvprintw(row, cpu_bar_start, "[");
        attroff(COLOR_PAIR(4));

        int drawn = 0;
        attron(COLOR_PAIR(1));
        for (int j = 0; j < usr_bars && drawn < cpu_bars; j++, drawn++)
          addch('#');
        attroff(COLOR_PAIR(1));
        attron(COLOR_PAIR(2));
        for (int j = 0; j < sys_bars && drawn < cpu_bars; j++, drawn++)
          addch('#');
        attroff(COLOR_PAIR(2));
        attron(COLOR_PAIR(3));
        for (int j = 0; j < nice_bars && drawn < cpu_bars; j++, drawn++)
          addch('#');
        attroff(COLOR_PAIR(3));
        for (int j = drawn; j < bar_width; j++)
          addch(' ');
        attron(COLOR_PAIR(4));
        mvprintw(row, cpu_bar_start + 1 + bar_width, "]");
        attroff(COLOR_PAIR(4));

        /* Print CPU load right-aligned inside CPU bar */
        mvprintw(row, cpu_bar_start + 1 + bar_width - 6, "%5.1f%%", hosts[i].cpu_usage);

        /* Draw Memory bar */
        if (hosts[i].mem_initialized && hosts[i].mem_total > 0) {
          int mem_used_bars = (int)((double)hosts[i].mem_used / hosts[i].mem_total * bar_width);
          int mem_buff_bars = (hosts[i].mem_buffers > 0) ? (int)((double)hosts[i].mem_buffers / hosts[i].mem_total * bar_width) : 0;
          int mem_total_bars = mem_used_bars + mem_buff_bars;
          if (mem_total_bars > bar_width) {
            mem_buff_bars = bar_width - mem_used_bars;
            if (mem_buff_bars < 0) mem_buff_bars = 0;
            mem_total_bars = bar_width;
          }

          attron(COLOR_PAIR(4));
          mvprintw(row, mem_bar_start, "[");
          attroff(COLOR_PAIR(4));

          attron(COLOR_PAIR(1));
          for (int j = 0; j < mem_used_bars; j++)
            addch('#');
          attroff(COLOR_PAIR(1));
          attron(COLOR_PAIR(6));
          for (int j = 0; j < mem_buff_bars; j++)
            addch('#');
          attroff(COLOR_PAIR(6));
          for (int j = mem_total_bars; j < bar_width; j++)
            addch(' ');
          attron(COLOR_PAIR(4));
          mvprintw(row, mem_bar_start + 1 + bar_width, "]");
          attroff(COLOR_PAIR(4));

          /* Print memory usage right-aligned inside memory bar */
          mvprintw(row, mem_bar_start + 1 + bar_width - 7, "%6.1f%%", hosts[i].mem_usage);
        } else {
          attron(COLOR_PAIR(4));
          mvprintw(row, mem_bar_start, "[");
          mvprintw(row, mem_bar_start + 1 + bar_width, "]");
          attroff(COLOR_PAIR(4));
          mvprintw(row, mem_bar_start + 1, "%-*s", bar_width, "...");
        }

        /* Draw GPU bar; hosts without a GPU show n/a */
        if (show_gpu) {
          attron(COLOR_PAIR(4));
          mvprintw(row, gpu_bar_start, "[");
          attroff(COLOR_PAIR(4));

          if (hosts[i].gpu_initialized) {
            int gpu_bars = (int)(hosts[i].gpu_usage / 100.0 * bar_width);
            if (gpu_bars > bar_width)
              gpu_bars = bar_width;

            attron(COLOR_PAIR(1));
            for (int j = 0; j < gpu_bars; j++)
              addch('#');
            attroff(COLOR_PAIR(1));
            for (int j = gpu_bars; j < bar_width; j++)
              addch(' ');

            /* Print GPU load right-aligned inside GPU bar */
            mvprintw(row, gpu_bar_start + 1 + bar_width - 4, "%3.0f%%", hosts[i].gpu_usage);
          } else {
            mvprintw(row, gpu_bar_start + 1, "%-*s", bar_width, "n/a");
          }

          attron(COLOR_PAIR(4));
          mvprintw(row, gpu_bar_start + 1 + bar_width, "]");
          attroff(COLOR_PAIR(4));
        }
      }

      if (compact_mode) {
        attron(COLOR_PAIR(7));
        if (hosts[i].temp_initialized) {
          mvprintw(row, pct_col, "%3.0f %3.0f %3.0f %3.0f %s",
                   hosts[i].user_pct, hosts[i].system_pct,
                   hosts[i].idle_pct, hosts[i].cpu_temp, locked ? " " : spinner);
        } else {
          mvprintw(row, pct_col, "%3.0f %3.0f %3.0f %4s %s",
                   hosts[i].user_pct, hosts[i].system_pct,
                   hosts[i].idle_pct, "---", locked ? " " : spinner);
        }
        attroff(COLOR_PAIR(7));
      } else {
        if (hosts[i].temp_initialized) {
          attron(COLOR_PAIR(7));
          mvprintw(row, pct_col, "%5.1f %5.1f %5.1f %5.1f %5.1f %s",
                   hosts[i].user_pct, hosts[i].system_pct,
                   hosts[i].nice_pct, hosts[i].idle_pct, hosts[i].cpu_temp, locked ? " " : spinner);
          attroff(COLOR_PAIR(7));
        } else {
          attron(COLOR_PAIR(7));
          mvprintw(row, pct_col, "%5.1f %5.1f %5.1f %5.1f %4s %s",
                   hosts[i].user_pct, hosts[i].system_pct,
                   hosts[i].nice_pct, hosts[i].idle_pct, "---", locked ? " " : spinner);
          attroff(COLOR_PAIR(7));
        }
      }
    } else {
      if (show_bars) {
        attron(COLOR_PAIR(4));
        mvprintw(row, cpu_bar_start, "[");
        mvprintw(row, cpu_bar_start + 1 + bar_width, "]");
        attroff(COLOR_PAIR(4));
        mvprintw(row, cpu_bar_start + 1, "%-*s", bar_width, "...");

        attron(COLOR_PAIR(4));
        mvprintw(row, mem_bar_start, "[");
        mvprintw(row, mem_bar_start + 1 + bar_width, "]");
        attroff(COLOR_PAIR(4));
        mvprintw(row, mem_bar_start + 1, "%-*s", bar_width, "...");

        if (show_gpu) {
          attron(COLOR_PAIR(4));
          mvprintw(row, gpu_bar_start, "[");
          mvprintw(row, gpu_bar_start + 1 + bar_width, "]");
          attroff(COLOR_PAIR(4));
          mvprintw(row, gpu_bar_start + 1, "%-*s", bar_width, "...");
        }
      }

      attron(COLOR_PAIR(7));
      if (compact_mode) {
        mvprintw(row, pct_col, "%-*s%s", 20, "connecting...", locked ? " " : spinner);
      } else {
        mvprintw(row, pct_col, "%-*s%s", show_bars ? 47 : 20, "connecting...", locked ? " " : spinner);
      }
      attroff(COLOR_PAIR(7));
    }

    if (locked)
      pthread_mutex_unlock(&hosts[i].lock);
  }

  /* Draw footer line */
  int footer_row = max_y - 1;
  attron(COLOR_PAIR(5));
  mvhline(footer_row, 0, ' ', max_x);
  mvprintw(footer_row, 0, " ESC Quit");
  /* Right-aligned version and name */
  char footer_right[64];
  snprintf(footer_right, sizeof(footer_right), "Cluster CPU/Mem monitor %s", VERSION);
  int right_col = max_x - (int)strlen(footer_right) - 1;
  if (right_col > 10)
    mvprintw(footer_row, right_col, "%s", footer_right);

  refresh();
  return 0;
}

/* Main entry point */
int main(int argc, char *argv[]) {
  double interval = 0.5;
  const char *hostfile = DEFAULT_HOSTS_FILE;
  int opt;
  int steps;

  static struct option long_options[] = {
      {"interval", required_argument, 0, 'n'},
      {"hostfile", required_argument, 0, 'f'},
      {"hosts", required_argument, 0, 'H'},
      {"threads", required_argument, 0, 't'},
      {"help", no_argument, 0, 'h'},
      {"version", no_argument, 0, 'v'},
      {0, 0, 0, 0}};

  while ((opt = getopt_long(argc, argv, "n:f:H:t:hv", long_options, NULL)) !=
         -1) {
    switch (opt) {
    case 'n':
      interval = atof(optarg);
      if (interval <= 0)
        interval = 0.5;
      break;
    case 'f':
      hostfile = optarg;
      break;
    case 'H':
      parse_hosts_list(optarg);
      break;
    case 't':
      num_threads = atoi(optarg);
      if (num_threads < 1)
        num_threads = 1;
      if (num_threads > 100)
        num_threads = 100;
      break;
    case 'h':
      print_help(argv[0]);
      return 0;
    case 'v':
      print_version();
      return 0;
    default:
      print_help(argv[0]);
      return 1;
    }
  }

  /* Read hosts from file if specified (silently skip if file doesn't exist) */
  if (hostfile) {
    if (read_hosts(hostfile) < 0) {
      /* If default hosts file not found, try ~/.ccmrc then /etc/ccm.conf */
      if (strcmp(hostfile, DEFAULT_HOSTS_FILE) == 0) {
        const char *home = getenv("HOME");
        if (home) {
          char home_hosts[MAX_HOSTNAME];
          snprintf(home_hosts, sizeof(home_hosts), "%s/%s", home,
                   HOME_HOSTS_FILE);
          if (read_hosts(home_hosts) < 0) {
            read_hosts(FALLBACK_HOSTS_FILE);
          }
        } else {
          read_hosts(FALLBACK_HOSTS_FILE);
        }
      }
    }
  }

  /* Ensure we have at least one host to monitor */
  if (host_count == 0) {
    fprintf(stderr,
            "Error: No hosts specified. Provide hostnames in %s, ~/%s, %s, or "
            "use -H/--hosts option.\n",
            DEFAULT_HOSTS_FILE, HOME_HOSTS_FILE, FALLBACK_HOSTS_FILE);
    return 1;
  }

  /* Store interval for background thread */
  update_interval = interval;

  /* Drop setuid/setgid privileges permanently */
  if (setregid(getgid(), getgid()) == -1) {
    perror("setregid");
    return 1;
  }
  if (setreuid(getuid(), getuid()) == -1) {
    perror("setreuid");
    return 1;
  }

  /* Initialize locale: POSIX for safe parsing, LC_CTYPE for UTF-8 ncurses */
  setlocale(LC_ALL, "POSIX");
  setlocale(LC_CTYPE, "");
  initscr();
  start_color();
  init_pair(1, COLOR_GREEN, COLOR_BLACK);  /* User CPU / Mem used */
  init_pair(2, COLOR_RED, COLOR_BLACK);    /* System CPU time */
  init_pair(3, COLOR_YELLOW, COLOR_BLACK); /* Nice CPU / Mem cached */
  init_pair(4, COLOR_BLUE, COLOR_BLACK);   /* Bracket delimiters */
  init_pair(5, COLOR_BLACK, COLOR_GREEN);  /* Header/footer background */
  init_pair(6, COLOR_CYAN, COLOR_BLACK);   /* Mem buffers/cache */
  init_pair(7, COLOR_WHITE, COLOR_BLACK);  /* Stats text */
  cbreak();
  noecho();
  nodelay(stdscr, TRUE);
  keypad(stdscr, TRUE);
  curs_set(0);

  /* Start background thread for updating CPU data */
  pthread_t update_thread;
  pthread_create(&update_thread, NULL, update_thread_func, NULL);

  /* Wait for initial SSH data collection before first display */
  while (1) {
    int ready = 0;
    for (int i = 0; i < host_count; i++) {
      pthread_mutex_lock(&hosts[i].lock);
      if (hosts[i].initialized || hosts[i].ssh_failed) ready++;
      pthread_mutex_unlock(&hosts[i].lock);
    }
    if (ready == host_count) break;
    napms(50);
  }

  /* Calculate number of steps for the display update interval */
  steps = (int)(interval * 10);
  if (steps < 1)
    steps = 1;

  /* Main event loop - update display and check for input */
  while (1) {
    int ch = getch();
    if (ch == 'q' || ch == 'Q' || ch == 27)
      break;

    /* Update display with current terminal size (handles resize automatically) */
    if (update_display() != 0) {
      int min_width = HOSTNAME_COL_WIDTH + 2 + STATS_COL_WIDTH;
      int min_height = 3;
      int max_y, max_x;
      getmaxyx(stdscr, max_y, max_x);
      running = 0;
      pthread_join(update_thread, NULL);
      for (int i = 0; i < host_count; i++) {
        pthread_mutex_destroy(&hosts[i].lock);
      }
      endwin();
      fprintf(stderr, "Error: Terminal too small (need at least %dx%d, got %dx%d)\n",
              min_width, min_height, max_x, max_y);
      return 1;
    }

    /* Sleep for update interval, checking for input periodically */
    for (int s = 0; s < steps; s++) {
      napms(100);
      ch = getch();
      if (ch == 'q' || ch == 'Q' || ch == 27)
        break;
      if (ch == KEY_RESIZE) {
        /* Immediately update display with new terminal size */
        break;
      }
    }
    if (ch == 'q' || ch == 'Q' || ch == 27)
      break;
  }

  /* Signal background thread to stop and wait for it */
  running = 0;
  pthread_join(update_thread, NULL);

  /* Destroy per-host mutexes */
  for (int i = 0; i < host_count; i++) {
    pthread_mutex_destroy(&hosts[i].lock);
  }

  /* Clean up ncurses */
  endwin();
  return 0;
}
