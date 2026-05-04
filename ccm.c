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

#include <getopt.h>
#include <locale.h>
#include <ncursesw/ncurses.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VERSION "0.0.4"
#define DEFAULT_HOSTS_FILE "hosts.txt"
#define HOME_HOSTS_FILE ".ccmrc"
#define FALLBACK_HOSTS_FILE "/etc/ccm.conf"

#define HOSTS_FILE "hosts.txt"
#define MAX_HOSTS 10000
#define MAX_HOSTNAME 256
#define HOSTNAME_COL_WIDTH 16
#define STATS_COL_WIDTH 48 /* " Load%  usr% sys% nice% idle%  Mem%  Temp   U" */
#define STATS_COL_WIDTH_COMPACT 28 /* " Ld%  usr sys idle  Mem% Tmp U" */

/* Host information structure */
typedef struct {
  char hostname[MAX_HOSTNAME];
  double cpu_usage;          /* Total CPU usage percentage */
  double user_pct, nice_pct; /* User and nice time percentages */
  double system_pct, idle_pct,
      iowait_pct; /* System, idle, and I/O wait percentages */
  unsigned long long prev_user, prev_nice,
      prev_system; /* Previous CPU counters */
  unsigned long long prev_idle, prev_iowait,
      prev_total;  /* Previous idle and total */
  int initialized; /* Flag if previous values are initialized */
  int ssh_failed;    /* Flag if SSH connection failed */
  double mem_usage;  /* Memory utilization percentage */
  int mem_initialized; /* Flag if memory values are initialized */
  unsigned long long mem_total, mem_used, mem_shared, mem_buffers, mem_cached;
  double cpu_temp;
  int temp_initialized;
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

/* Read hosts from a file, one hostname per line */
int read_hosts(const char *filename) {
  FILE *fp = fopen(filename, "r");
  if (!fp)
    return -1;

  char line[MAX_HOSTNAME];

  while (fgets(line, sizeof(line), fp) && host_count < MAX_HOSTS) {
    line[strcspn(line, "\n")] = 0;
    if (strlen(line) > 0 && line[0] != '#') {
      snprintf(hosts[host_count].hostname, MAX_HOSTNAME, "%s", line);
      hosts[host_count].initialized = 0;
      hosts[host_count].ssh_failed = 0;
      hosts[host_count].mem_usage = 0.0;
      hosts[host_count].mem_initialized = 0;
      hosts[host_count].cpu_temp = 0.0;
      hosts[host_count].temp_initialized = 0;
      pthread_mutex_init(&hosts[host_count].lock, NULL);
      host_count++;
    }
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

    if (strlen(token) > 0) {
      snprintf(hosts[host_count].hostname, MAX_HOSTNAME, "%s", token);
      hosts[host_count].initialized = 0;
      hosts[host_count].ssh_failed = 0;
      hosts[host_count].mem_usage = 0.0;
      hosts[host_count].mem_initialized = 0;
      hosts[host_count].cpu_temp = 0.0;
      hosts[host_count].temp_initialized = 0;
      pthread_mutex_init(&hosts[host_count].lock, NULL);
      host_count++;
    }

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

/* Get CPU and memory usage via a single SSH command to the remote host */
int get_host_info(const char *hostname, HostInfo *host) {
  char cmd[512];
  FILE *fp;
  char line[256];

  snprintf(
      cmd, sizeof(cmd),
      "ssh -x -T -o ConnectTimeout=1 -o ServerAliveInterval=1 -o ServerAliveCountMax=1 -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o GSSAPIAuthentication=no %s "
      "\"{ cat /proc/stat 2>/dev/null | head -1; free 2>/dev/null | grep '^Mem:'; cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 'N/A'; }\" 2>/dev/null",
      hostname);

  fp = popen(cmd, "r");
  if (!fp) {
    host->ssh_failed = 1;
    return -1;
  }

  /* Parse CPU line */
  if (!fgets(line, sizeof(line), fp)) {
    pclose(fp);
    host->ssh_failed = 1;
    return -1;
  }

  unsigned long long user, nice, system, idle, iowait;
  if (sscanf(line, "cpu %llu %llu %llu %llu %llu", &user, &nice, &system, &idle,
             &iowait) < 5) {
    pclose(fp);
    host->ssh_failed = 1;
    return -1;
  }

  /* Parse memory line */
  if (!fgets(line, sizeof(line), fp)) {
    pclose(fp);
    host->ssh_failed = 1;
    return -1;
  }

  unsigned long long mem_total = 0, mem_used = 0, mem_free = 0, mem_shared = 0, mem_buff_cache = 0, mem_available = 0;
  if (sscanf(line, "Mem: %llu %llu %llu %llu %llu %llu", &mem_total, &mem_used, &mem_free, &mem_shared, &mem_buff_cache, &mem_available) >= 6) {
    if (mem_total > 0) {
      host->mem_total = mem_total;
      host->mem_used = mem_used;
      host->mem_shared = mem_shared;
      host->mem_buffers = mem_buff_cache;
      host->mem_cached = 0;
      host->mem_usage = ((double)mem_used / mem_total) * 100.0;
      host->mem_initialized = 1;
    }
  }

  /* Parse temperature line (millidegrees Celsius) */
  if (!fgets(line, sizeof(line), fp)) {
    pclose(fp);
    host->ssh_failed = 1;
    return -1;
  }
  pclose(fp);

  int temp_raw;
  if (sscanf(line, "%d", &temp_raw) == 1 && temp_raw > 0) {
    host->cpu_temp = temp_raw / 1000.0;
    host->temp_initialized = 1;
  } else {
    host->cpu_temp = 0.0;
    host->temp_initialized = 0;
  }

  host->ssh_failed = 0;

  unsigned long long total = user + nice + system + idle + iowait;

  if (!host->initialized) {
    host->prev_user = user;
    host->prev_nice = nice;
    host->prev_system = system;
    host->prev_idle = idle;
    host->prev_iowait = iowait;
    host->prev_total = total;
    host->initialized = 1;
    return -1;
  }

  unsigned long long user_delta = user - host->prev_user;
  unsigned long long nice_delta = nice - host->prev_nice;
  unsigned long long system_delta = system - host->prev_system;
  unsigned long long idle_delta = idle - host->prev_idle;
  unsigned long long iowait_delta = iowait - host->prev_iowait;
  unsigned long long total_delta = total - host->prev_total;

  if (total_delta > 0) {
    host->cpu_usage =
        ((double)(total_delta - idle_delta - iowait_delta) / total_delta) *
        100.0;
    host->user_pct = (double)user_delta / total_delta * 100.0;
    host->nice_pct = (double)nice_delta / total_delta * 100.0;
    host->system_pct = (double)system_delta / total_delta * 100.0;
    host->idle_pct = (double)idle_delta / total_delta * 100.0;
    host->iowait_pct = (double)iowait_delta / total_delta * 100.0;
  } else {
    host->cpu_usage = 0.0;
    host->user_pct = host->nice_pct = host->system_pct = 0.0;
    host->idle_pct = host->iowait_pct = 0.0;
  }

  host->prev_user = user;
  host->prev_nice = nice;
  host->prev_system = system;
  host->prev_idle = idle;
  host->prev_iowait = iowait;
  host->prev_total = total;

  return 0;
}

/* Print help message */
void print_help(const char *prog) {
  printf("Usage: %s [OPTIONS]\n", prog);
  printf("Monitor CPU and memory usage across multiple hosts via SSH.\n\n");
  printf("Options:\n");
  printf("  -n SEC           Update interval in seconds (default: 0.5)\n");
  printf("  --hostfile FILE  Use specified hosts file (default: %s, fallback: "
         "~/%s, then %s)\n",
         DEFAULT_HOSTS_FILE, HOME_HOSTS_FILE, FALLBACK_HOSTS_FILE);
  printf("  -H, --hosts LIST Comma-separated list of hosts to monitor\n");
  printf("  -t, --threads N  Number of parallel SSH threads (default: 4)\n");
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

    stats_width = wide_stats_width;
    mem_bar_start = cpu_bar_start + bar_width + 4;
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
          int mem_buff_bars = (int)((double)hosts[i].mem_buffers / hosts[i].mem_total * bar_width);
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

  /* Initialize ncurses */
  setlocale(LC_ALL, "");
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
