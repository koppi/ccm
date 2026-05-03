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
#include <ncurses.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VERSION "0.0.1"
#define DEFAULT_HOSTS_FILE "hosts.txt"
#define HOME_HOSTS_FILE ".ccmrc"
#define FALLBACK_HOSTS_FILE "/etc/ccm.conf"

#define HOSTS_FILE "hosts.txt"
#define MAX_HOSTS 10000
#define MAX_HOSTNAME 256
#define HOSTNAME_COL_WIDTH 16
#define PCT_COL_WIDTH 36 /* "  Load%  usr%  sys% nice% idle%" */

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
  int ssh_failed;  /* Flag if SSH connection failed */
} HostInfo;

/* Forward declarations */
int get_cpu_usage(const char *hostname, HostInfo *host);

/* Global array of hosts and host count */
HostInfo hosts[MAX_HOSTS];
int host_count = 0;
int running = 1;
double update_interval = 0.5;
int num_threads = 4;
pthread_mutex_t hosts_mutex = PTHREAD_MUTEX_INITIALIZER;

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
    get_cpu_usage(hosts[i].hostname, &hosts[i]);
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

    pthread_mutex_lock(&hosts_mutex);

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

    pthread_mutex_unlock(&hosts_mutex);

    /* Sleep in small increments to check running flag */
    for (int s = 0; s < 10 && running; s++) {
      napms((int)(update_interval * 100));
    }
  }
  return NULL;
}

/* Get CPU usage via SSH by reading /proc/stat from remote host */
int get_cpu_usage(const char *hostname, HostInfo *host) {
  char cmd[512];
  FILE *fp;
  char line[256];
  unsigned long long user, nice, system, idle, iowait;

  snprintf(
      cmd, sizeof(cmd),
      "ssh -o ConnectTimeout=2 -o BatchMode=yes -o StrictHostKeyChecking=no %s "
      "\"cat /proc/stat 2>/dev/null | head -1\" 2>/dev/null",
      hostname);

  fp = popen(cmd, "r");
  if (!fp) {
    host->ssh_failed = 1;
    return -1;
  }

  if (!fgets(line, sizeof(line), fp)) {
    pclose(fp);
    host->ssh_failed = 1;
    return -1;
  }
  pclose(fp);

  if (sscanf(line, "cpu %llu %llu %llu %llu %llu", &user, &nice, &system, &idle,
             &iowait) < 5) {
    host->ssh_failed = 1;
    return -1;
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
  printf("Monitor CPU usage across multiple hosts via SSH.\n\n");
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

/* Update the ncurses display with current CPU usage for all hosts */
void update_display() {
  clear();

  int max_y, max_x;
  getmaxyx(stdscr, max_y, max_x);

  pthread_mutex_lock(&hosts_mutex);

  mvprintw(0, 0, "Cluster CPU monitor (Press q or ESC to quit)");

  /* Calculate dynamic bar width based on terminal width */
  int bar_start_col = HOSTNAME_COL_WIDTH + 2; /* After hostname + 2 spaces */
  int pct_col = max_x - PCT_COL_WIDTH;
  if (pct_col < bar_start_col + 10)
    pct_col = bar_start_col + 10;

  int bar_width = pct_col - bar_start_col - 3; /* -3 for "[ ]" */
  if (bar_width < 10)
    bar_width = 10;
  if (bar_width > max_x - bar_start_col - 3)
    bar_width = max_x - bar_start_col - 3;

  int row = 1;

  /* Print header row */
  mvprintw(row, 0, "%-*s", HOSTNAME_COL_WIDTH, "Hostname");
  mvprintw(row, bar_start_col, "[%-*s]", bar_width, "CPU load");
  mvprintw(row, pct_col, "%6s %5s %5s %5s %5s", "Load%", "usr%", "sys%",
           "nice%", "idle%");

  /* Print separator line */
  row = 2;
  for (int i = 0; i <= max_x; i++)
    addch('-');

  /* Print each host's CPU usage */
  for (int i = 0; i < host_count && (i + 3) < max_y; i++) {
    row = i + 3;
    char display_name[HOSTNAME_COL_WIDTH + 1];
    strncpy(display_name, hosts[i].hostname, HOSTNAME_COL_WIDTH);
    display_name[HOSTNAME_COL_WIDTH] = '\0';
    mvprintw(row, 0, "%-*s", HOSTNAME_COL_WIDTH, display_name);

    if (hosts[i].ssh_failed) {
      mvprintw(row, bar_start_col, "[%-*s]", bar_width, "SSH FAILED");
      mvprintw(row, pct_col, "%6s  %5s %5s %5s %5s", "--.-", "--.-", "--.-",
               "--.-", "--.-");
    } else if (hosts[i].initialized) {
      int bars = (int)(hosts[i].cpu_usage / 100.0 * bar_width);
      if (bars > bar_width)
        bars = bar_width;

      int usr_bars = (int)(hosts[i].user_pct / 100.0 * bar_width);
      int sys_bars = (int)(hosts[i].system_pct / 100.0 * bar_width);
      int nice_bars = (int)(hosts[i].nice_pct / 100.0 * bar_width);

      mvprintw(row, bar_start_col, "[");

      int drawn = 0;
      attron(COLOR_PAIR(1));
      for (int j = 0; j < usr_bars && drawn < bars; j++, drawn++)
        addch('#');
      attroff(COLOR_PAIR(1));

      attron(COLOR_PAIR(2));
      for (int j = 0; j < sys_bars && drawn < bars; j++, drawn++)
        addch('#');
      attroff(COLOR_PAIR(2));

      attron(COLOR_PAIR(3));
      for (int j = 0; j < nice_bars && drawn < bars; j++, drawn++)
        addch('#');
      attroff(COLOR_PAIR(3));

      for (int j = drawn; j < bar_width; j++)
        addch(' ');

      mvprintw(row, bar_start_col + 1 + bar_width, "]");

      mvprintw(row, pct_col, "%6.1f%% %5.1f %5.1f %5.1f %5.1f",
               hosts[i].cpu_usage, hosts[i].user_pct, hosts[i].system_pct,
               hosts[i].nice_pct, hosts[i].idle_pct);
    } else {
      mvprintw(row, bar_start_col, "[%-*s] connecting...", bar_width, "...");
    }
  }

  pthread_mutex_unlock(&hosts_mutex);

  refresh();
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
  initscr();
  start_color();
  init_pair(1, COLOR_GREEN, COLOR_BLACK);  /* User CPU time */
  init_pair(2, COLOR_RED, COLOR_BLACK);    /* System CPU time */
  init_pair(3, COLOR_YELLOW, COLOR_BLACK); /* Nice CPU time */
  cbreak();
  noecho();
  nodelay(stdscr, TRUE);
  keypad(stdscr, TRUE);

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
    if (ch == KEY_RESIZE) {
      clear();
      refresh();
    }

    update_display();

    for (int s = 0; s < steps; s++) {
      napms(100);
      ch = getch();
      if (ch == 'q' || ch == 'Q' || ch == 27)
        break;
      if (ch == KEY_RESIZE) {
        clear();
        break;
      }
    }
    if (ch == 'q' || ch == 'Q' || ch == 27)
      break;
  }

  /* Signal background thread to stop and wait for it */
  running = 0;
  pthread_join(update_thread, NULL);
  pthread_mutex_destroy(&hosts_mutex);

  /* Clean up ncurses */
  endwin();
  return 0;
}
