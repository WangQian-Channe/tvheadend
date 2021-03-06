/*
 *  TVheadend
 *  Copyright (C) 2007 - 2010 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <syslog.h>
#include <limits.h>
#include <time.h>
#include <locale.h>

#include <pwd.h>
#include <grp.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include "tvheadend.h"
#include "api.h"
#include "tcp.h"
#include "access.h"
#include "http.h"
#include "upnp.h"
#include "webui/webui.h"
#include "epggrab.h"
#include "spawn.h"
#include "subscriptions.h"
#include "service_mapper.h"
#include "descrambler.h"
#include "dvr/dvr.h"
#include "htsp_server.h"
#include "avahi.h"
#include "bonjour.h"
#include "input.h"
#include "service.h"
#include "trap.h"
#include "settings.h"
#include "config.h"
#include "idnode.h"
#include "imagecache.h"
#include "timeshift.h"
#include "fsmonitor.h"
#include "lang_codes.h"
#include "esfilter.h"
#include "intlconv.h"
#include "dbus.h"
#if ENABLE_LIBAV
#include "libav.h"
#include "plumbing/transcoding.h"
#endif

#ifdef PLATFORM_LINUX
#include <sys/prctl.h>
#endif
#include <openssl/ssl.h>
#include <openssl/conf.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/engine.h>

pthread_t main_tid;

/* Command line option struct */
typedef struct {
  const char  sopt;
  const char *lopt;
  const char *desc;
  enum {
    OPT_STR,
    OPT_INT,
    OPT_BOOL, 
    OPT_STR_LIST,
  }          type;
  void       *param;
} cmdline_opt_t;

static cmdline_opt_t* cmdline_opt_find
  ( cmdline_opt_t *opts, int num, const char *arg )
{
  int i;
  int isshort = 0;

  if (strlen(arg) < 2 || *arg != '-')
    return NULL;
  arg++;

  if (strlen(arg) == 1)
    isshort = 1;
  else if (*arg == '-')
    arg++;
  else
    return NULL;

  for (i = 0; i < num; i++) {
    if (!opts[i].lopt) continue;
    if (isshort && opts[i].sopt == *arg)
      return &opts[i];
    if (!isshort && !strcmp(opts[i].lopt, arg))
      return &opts[i];
  }

  return NULL;
}

/*
 * Globals
 */
int              tvheadend_running;
int              tvheadend_webui_port;
int              tvheadend_webui_debug;
int              tvheadend_htsp_port;
int              tvheadend_htsp_port_extra;
const char      *tvheadend_cwd;
const char      *tvheadend_webroot;
const tvh_caps_t tvheadend_capabilities[] = {
#if ENABLE_CWC
  { "cwc", NULL },
#endif
#if ENABLE_CAPMT
  { "capmt", NULL },
#endif
#if ENABLE_V4L
  { "v4l", NULL },
#endif
#if ENABLE_LINUXDVB
  { "linuxdvb", NULL },
#endif
#if ENABLE_SATIP_CLIENT
  { "satip_client", NULL },
#endif
#if ENABLE_LIBAV
  { "transcoding", &transcoding_enabled },
#endif
#if ENABLE_IMAGECACHE
  { "imagecache", (uint32_t*)&imagecache_conf.enabled },
#endif
#if ENABLE_TIMESHIFT
  { "timeshift", &timeshift_enabled },
#endif
#if ENABLE_TRACE
  { "trace",     NULL },
#endif
  { NULL, NULL }
};

pthread_mutex_t global_lock;
pthread_mutex_t ffmpeg_lock;
pthread_mutex_t fork_lock;
pthread_mutex_t atomic_lock;

/*
 * Locals
 */
static LIST_HEAD(, gtimer) gtimers;
static pthread_cond_t gtimer_cond;

static void
handle_sigpipe(int x)
{
  return;
}

static void
handle_sigill(int x)
{
  /* Note that on some platforms, the SSL library tries */
  /* to determine the CPU capabilities with possible */
  /* unknown instructions */
  tvhwarn("CPU", "Illegal instruction handler (might be OK)");
  signal(SIGILL, handle_sigill);
}

void
doexit(int x)
{
  if (pthread_self() != main_tid)
    pthread_kill(main_tid, SIGTERM);
  pthread_cond_signal(&gtimer_cond);
  tvheadend_running = 0;
  signal(x, doexit);
}

static int
get_user_groups (const struct passwd *pw, gid_t* glist, size_t gmax)
{
  int num = 0;
#if !ENABLE_ANDROID
  struct group *gr;
  char **mem;
  glist[num++] = pw->pw_gid;
  for ( gr = getgrent(); (gr != NULL) && (num < gmax); gr = getgrent() ) {
    if (gr->gr_gid == pw->pw_gid) continue;
    for (mem = gr->gr_mem; *mem; mem++) {
      if(!strcmp(*mem, pw->pw_name)) glist[num++] = gr->gr_gid;
    }
  }
#endif
  return num;
}

/**
 *
 */
static int
gtimercmp(gtimer_t *a, gtimer_t *b)
{
  if(a->gti_expire.tv_sec  < b->gti_expire.tv_sec)
    return -1;
  if(a->gti_expire.tv_sec  > b->gti_expire.tv_sec)
    return 1;
  if(a->gti_expire.tv_nsec < b->gti_expire.tv_nsec)
    return -1;
  if(a->gti_expire.tv_nsec > b->gti_expire.tv_nsec)
    return 1;
 return 0;
}

/**
 *
 */
void
gtimer_arm_abs2
  (gtimer_t *gti, gti_callback_t *callback, void *opaque, struct timespec *when)
{
  lock_assert(&global_lock);

  if (gti->gti_callback != NULL)
    LIST_REMOVE(gti, gti_link);

  gti->gti_callback = callback;
  gti->gti_opaque   = opaque;
  gti->gti_expire   = *when;

  LIST_INSERT_SORTED(&gtimers, gti, gti_link, gtimercmp);

  //tvhdebug("gtimer", "%p @ %ld.%09ld", gti, when->tv_sec, when->tv_nsec);

  if (LIST_FIRST(&gtimers) == gti)
    pthread_cond_signal(&gtimer_cond); // force timer re-check
}

/**
 *
 */
void
gtimer_arm_abs
  (gtimer_t *gti, gti_callback_t *callback, void *opaque, time_t when)
{
  struct timespec ts;
  ts.tv_nsec = 0;
  ts.tv_sec  = when;
  gtimer_arm_abs2(gti, callback, opaque, &ts);
}

/**
 *
 */
void
gtimer_arm(gtimer_t *gti, gti_callback_t *callback, void *opaque, int delta)
{
  gtimer_arm_abs(gti, callback, opaque, dispatch_clock + delta);
}

/**
 *
 */
void
gtimer_arm_ms
  (gtimer_t *gti, gti_callback_t *callback, void *opaque, long delta_ms )
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += (1000000 * delta_ms);
  ts.tv_sec  += (ts.tv_nsec / 1000000000);
  ts.tv_nsec %= 1000000000;
  gtimer_arm_abs2(gti, callback, opaque, &ts);
}

/**
 *
 */
void
gtimer_disarm(gtimer_t *gti)
{
  if(gti->gti_callback) {
    //tvhdebug("gtimer", "%p disarm", gti);
    LIST_REMOVE(gti, gti_link);
    gti->gti_callback = NULL;
  }
}

/**
 * Show version info
 */
static void
show_version(const char *argv0)
{
  printf("%s: version %s\n", argv0, tvheadend_version);
  exit(0);
}

/**
 *
 */
static void
show_usage
  (const char *argv0, cmdline_opt_t *opts, int num, const char *err, ...)
{
  int i;
  char buf[256];
  printf("Usage: %s [OPTIONS]\n", argv0);
  for (i = 0; i < num; i++) {

    /* Section */
    if (!opts[i].lopt) {
      printf("\n%s\n\n",
            opts[i].desc);

    /* Option */
    } else {
      char sopt[4];
      char *desc, *tok;
      if (opts[i].sopt)
        snprintf(sopt, sizeof(sopt), "-%c,", opts[i].sopt);
      else
        strcpy(sopt, "   ");
      snprintf(buf, sizeof(buf), "  %s --%s", sopt, opts[i].lopt);
      desc = strdup(opts[i].desc);
      tok  = strtok(desc, "\n");
      while (tok) {
        printf("%-30s%s\n", buf, tok);
        tok = buf;
        while (*tok) {
          *tok = ' ';
          tok++;
        }
        tok = strtok(NULL, "\n");
      }
      free(desc);
    }
  }
  printf("\n");
  printf("For more information please visit the Tvheadend website:\n");
  printf("  https://tvheadend.org\n");
  printf("\n");
  exit(0);
}



/**
 *
 */
static void
mainloop(void)
{
  gtimer_t *gti;
  gti_callback_t *cb;
  struct timespec ts;

  while(tvheadend_running) {
    clock_gettime(CLOCK_REALTIME, &ts);

    /* 1sec stuff */
    if (ts.tv_sec > dispatch_clock) {
      dispatch_clock = ts.tv_sec;

      spawn_reaper(); /* reap spawned processes */

      comet_flush(); /* Flush idle comet mailboxes */
    }

    /* Global timers */
    pthread_mutex_lock(&global_lock);

    // TODO: there is a risk that if timers re-insert themselves to
    //       the top of the list with a 0 offset we could loop indefinitely
    
#if 0
    tvhdebug("gtimer", "now %ld.%09ld", ts.tv_sec, ts.tv_nsec);
    LIST_FOREACH(gti, &gtimers, gti_link)
      tvhdebug("gtimer", "  gti %p expire %ld.%08ld",
               gti, gti->gti_expire.tv_sec, gti->gti_expire.tv_nsec);
#endif

    while((gti = LIST_FIRST(&gtimers)) != NULL) {
      
      if ((gti->gti_expire.tv_sec > ts.tv_sec) ||
          ((gti->gti_expire.tv_sec == ts.tv_sec) &&
           (gti->gti_expire.tv_nsec > ts.tv_nsec))) {
        ts = gti->gti_expire;
        break;
      }

      cb = gti->gti_callback;
      //tvhdebug("gtimer", "%p callback", gti);

      LIST_REMOVE(gti, gti_link);
      gti->gti_callback = NULL;

      cb(gti->gti_opaque);
    }

    /* Bound wait */
    if ((LIST_FIRST(&gtimers) == NULL) || (ts.tv_sec > (dispatch_clock + 1))) {
      ts.tv_sec  = dispatch_clock + 1;
      ts.tv_nsec = 0;
    }

    /* Wait */
    //tvhdebug("gtimer", "wait till %ld.%09ld", ts.tv_sec, ts.tv_nsec);
    pthread_cond_timedwait(&gtimer_cond, &global_lock, &ts);
    pthread_mutex_unlock(&global_lock);
  }
}


/**
 *
 */
int
main(int argc, char **argv)
{
  int i;
  sigset_t set;
#if ENABLE_MPEGTS
  uint32_t adapter_mask = 0;
#endif
  int  log_level   = LOG_INFO;
  int  log_options = TVHLOG_OPT_MILLIS | TVHLOG_OPT_STDERR | TVHLOG_OPT_SYSLOG;
  const char *log_debug = NULL, *log_trace = NULL;
  char buf[512];

  main_tid = pthread_self();

  /* Setup global mutexes */
  pthread_mutex_init(&ffmpeg_lock, NULL);
  pthread_mutex_init(&fork_lock, NULL);
  pthread_mutex_init(&global_lock, NULL);
  pthread_mutex_init(&atomic_lock, NULL);
  pthread_cond_init(&gtimer_cond, NULL);

  /* Defaults */
  tvheadend_webui_port      = 9981;
  tvheadend_webroot         = NULL;
  tvheadend_htsp_port       = 9982;
  tvheadend_htsp_port_extra = 0;

  /* Command line options */
  int         opt_help         = 0,
              opt_version      = 0,
              opt_fork         = 0,
              opt_firstrun     = 0,
              opt_stderr       = 0,
              opt_syslog       = 0,
              opt_uidebug      = 0,
              opt_abort        = 0,
              opt_noacl        = 0,
              opt_fileline     = 0,
              opt_threadid     = 0,
              opt_ipv6         = 0,
              opt_tsfile_tuner = 0,
              opt_dump         = 0,
              opt_xspf         = 0,
              opt_dbus         = 0,
              opt_dbus_session = 0,
              opt_nobackup     = 0;
  const char *opt_config       = NULL,
             *opt_user         = NULL,
             *opt_group        = NULL,
             *opt_logpath      = NULL,
             *opt_log_debug    = NULL,
             *opt_log_trace    = NULL,
             *opt_pidpath      = "/var/run/tvheadend.pid",
#if ENABLE_LINUXDVB
             *opt_dvb_adapters = NULL,
#endif
             *opt_bindaddr     = NULL,
             *opt_subscribe    = NULL,
             *opt_user_agent   = NULL;
  str_list_t  opt_satip_xml    = { .max = 10, .num = 0, .str = calloc(10, sizeof(char*)) };
  str_list_t  opt_tsfile       = { .max = 10, .num = 0, .str = calloc(10, sizeof(char*)) };
  cmdline_opt_t cmdline_opts[] = {
    {   0, NULL,        "Generic Options",         OPT_BOOL, NULL         },
    { 'h', "help",      "Show this page",          OPT_BOOL, &opt_help    },
    { 'v', "version",   "Show version infomation", OPT_BOOL, &opt_version },

    {   0, NULL,        "Service Configuration",   OPT_BOOL, NULL         },
    { 'c', "config",    "Alternate config path",   OPT_STR,  &opt_config  },
    { 'B', "nobackup",  "Do not backup config tree at upgrade", OPT_BOOL, &opt_nobackup },
    { 'f', "fork",      "Fork and run as daemon",  OPT_BOOL, &opt_fork    },
    { 'u', "user",      "Run as user",             OPT_STR,  &opt_user    },
    { 'g', "group",     "Run as group",            OPT_STR,  &opt_group   },
    { 'p', "pid",       "Alternate pid path",      OPT_STR,  &opt_pidpath },
    { 'C', "firstrun",  "If no user account exists then create one with\n"
	                      "no username and no password. Use with care as\n"
	                      "it will allow world-wide administrative access\n"
	                      "to your Tvheadend installation until you edit\n"
	                      "the access-control from within the Tvheadend UI",
      OPT_BOOL, &opt_firstrun },
#if ENABLE_DBUS_1
    { 'U', "dbus",      "Enable DBus",
      OPT_BOOL, &opt_dbus },
    { 'e', "dbus_session", "DBus - use the session message bus instead system one",
      OPT_BOOL, &opt_dbus_session },
#endif
#if ENABLE_LINUXDVB
    { 'a', "adapters",  "Only use specified DVB adapters (comma separated)",
      OPT_STR, &opt_dvb_adapters },
#endif
#if ENABLE_SATIP_CLIENT
    {   0, "satip_xml", "URL with the SAT>IP server XML location",
      OPT_STR_LIST, &opt_satip_xml },
#endif
    {   0, NULL,         "Server Connectivity",    OPT_BOOL, NULL         },
    { '6', "ipv6",       "Listen on IPv6",         OPT_BOOL, &opt_ipv6    },
    { 'b', "bindaddr",   "Specify bind address",   OPT_STR,  &opt_bindaddr},
    {   0, "http_port",  "Specify alternative http port",
      OPT_INT, &tvheadend_webui_port },
    {   0, "http_root",  "Specify alternative http webroot",
      OPT_STR, &tvheadend_webroot },
    {   0, "htsp_port",  "Specify alternative htsp port",
      OPT_INT, &tvheadend_htsp_port },
    {   0, "htsp_port2", "Specify extra htsp port",
      OPT_INT, &tvheadend_htsp_port_extra },
    {   0, "useragent",  "Specify User-Agent header for the http client",
      OPT_STR, &opt_user_agent },
    {   0, "xspf",       "Use xspf playlist instead M3U",
      OPT_BOOL, &opt_xspf },

    {   0, NULL,        "Debug Options",           OPT_BOOL, NULL         },
    { 'd', "stderr",    "Enable debug on stderr",  OPT_BOOL, &opt_stderr  },
    { 's', "syslog",    "Enable debug to syslog",  OPT_BOOL, &opt_syslog  },
    { 'l', "logfile",   "Enable debug to file",    OPT_STR,  &opt_logpath },
    {   0, "debug",     "Enable debug subsystems", OPT_STR,  &opt_log_debug },
#if ENABLE_TRACE
    {   0, "trace",     "Enable trace subsystems", OPT_STR,  &opt_log_trace },
#endif
    {   0, "fileline",  "Add file and line numbers to debug", OPT_BOOL, &opt_fileline },
    {   0, "threadid",  "Add the thread ID to debug", OPT_BOOL, &opt_threadid },
    {   0, "uidebug",   "Enable webUI debug (non-minified JS)", OPT_BOOL, &opt_uidebug },
    { 'A', "abort",     "Immediately abort",       OPT_BOOL, &opt_abort   },
    { 'D', "dump",      "Enable coredumps for daemon", OPT_BOOL, &opt_dump },
    {   0, "noacl",     "Disable all access control checks",
      OPT_BOOL, &opt_noacl },
    { 'j', "join",      "Subscribe to a service permanently",
      OPT_STR, &opt_subscribe },


    { 0, NULL, "TODO: testing", OPT_BOOL, NULL },
    { 0, "tsfile_tuners", "Number of tsfile tuners", OPT_INT, &opt_tsfile_tuner },
    { 0, "tsfile", "tsfile input (mux file)", OPT_STR_LIST, &opt_tsfile },

  };

  /* Get current directory */
  tvheadend_cwd = dirname(dirname(tvh_strdupa(argv[0])));

  /* Set locale */
  setlocale(LC_ALL, "");
  setlocale(LC_NUMERIC, "C");

  /* make sure the timezone is set */
  tzset();

  /* Process command line */
  for (i = 1; i < argc; i++) {

    /* Find option */
    cmdline_opt_t *opt
      = cmdline_opt_find(cmdline_opts, ARRAY_SIZE(cmdline_opts), argv[i]);
    if (!opt)
      show_usage(argv[0], cmdline_opts, ARRAY_SIZE(cmdline_opts),
                 "invalid option specified [%s]", argv[i]);

    /* Process */
    if (opt->type == OPT_BOOL)
      *((int*)opt->param) = 1;
    else if (++i == argc)
      show_usage(argv[0], cmdline_opts, ARRAY_SIZE(cmdline_opts),
                 "option %s requires a value", opt->lopt);
    else if (opt->type == OPT_INT)
      *((int*)opt->param) = atoi(argv[i]);
    else if (opt->type == OPT_STR_LIST) {
      str_list_t *strl = opt->param;
      if (strl->num < strl->max)
        strl->str[strl->num++] = argv[i];
    }
    else
      *((char**)opt->param) = argv[i];

    /* Stop processing */
    if (opt_help)
      show_usage(argv[0], cmdline_opts, ARRAY_SIZE(cmdline_opts), NULL);
    if (opt_version)
      show_version(argv[0]);
  }

  /* Additional cmdline processing */
#if ENABLE_LINUXDVB
  if (!opt_dvb_adapters) {
    adapter_mask = ~0;
  } else {
    char *p, *e;
    char *r = NULL;
    char *dvb_adapters = strdup(opt_dvb_adapters);
    adapter_mask = 0x0;
    p = strtok_r(dvb_adapters, ",", &r);
    while (p) {
      int a = strtol(p, &e, 10);
      if (*e != 0 || a < 0 || a > 31) {
        tvhlog(LOG_ERR, "START", "Invalid adapter number '%s'", p);
        free(dvb_adapters);
        return 1;
      }
      adapter_mask |= (1 << a);
      p = strtok_r(NULL, ",", &r);
    }
    free(dvb_adapters);
    if (!adapter_mask) {
      tvhlog(LOG_ERR, "START", "No adapters specified!");
      return 1;
    }
  }
#endif
  if (tvheadend_webroot) {
    char *tmp;
    if (*tvheadend_webroot == '/')
      tmp = strdup(tvheadend_webroot);
    else {
      tmp = malloc(strlen(tvheadend_webroot)+2);
      *tmp = '/';
      strcpy(tmp+1, tvheadend_webroot);
    }
    if (tmp[strlen(tmp)-1] == '/')
      tmp[strlen(tmp)-1] = '\0';
    tvheadend_webroot = tmp;
  }
  tvheadend_webui_debug = opt_uidebug;

  /* Setup logging */
  if (isatty(2))
    log_options |= TVHLOG_OPT_DECORATE;
  if (opt_stderr || opt_syslog || opt_logpath) {
    if (!opt_log_trace && !opt_log_debug)
      log_debug      = "all";
    log_level      = LOG_DEBUG;
    if (opt_stderr)
      log_options   |= TVHLOG_OPT_DBG_STDERR;
    if (opt_syslog)
      log_options   |= TVHLOG_OPT_DBG_SYSLOG;
    if (opt_logpath)
      log_options   |= TVHLOG_OPT_DBG_FILE;
  }
  if (opt_fileline)
    log_options |= TVHLOG_OPT_FILELINE;
  if (opt_threadid)
    log_options |= TVHLOG_OPT_THREAD;
  if (opt_log_trace) {
    log_level  = LOG_TRACE;
    log_trace  = opt_log_trace;
  }
  if (opt_log_debug)
    log_debug  = opt_log_debug;
    
  tvhlog_init(log_level, log_options, opt_logpath);
  tvhlog_set_debug(log_debug);
  tvhlog_set_trace(log_trace);
  tvhinfo("main", "Log started");
 
  signal(SIGPIPE, handle_sigpipe); // will be redundant later
  signal(SIGILL, handle_sigill);   // see handler..

  tcp_server_preinit(opt_ipv6);
  http_server_init(opt_bindaddr);  // bind to ports only
  htsp_init(opt_bindaddr);	   // bind to ports only

  /* Daemonise */
  if(opt_fork) {
    const char *homedir;
    gid_t gid;
    uid_t uid;
    struct group  *grp = getgrnam(opt_group ?: "video");
    struct passwd *pw  = opt_user ? getpwnam(opt_user) : NULL;
    FILE   *pidfile    = fopen(opt_pidpath, "w+");

    if(grp != NULL) {
      gid = grp->gr_gid;
    } else {
      gid = 1;
    }

    if (pw != NULL) {
      if (getuid() != pw->pw_uid) {
        gid_t glist[10];
        int gnum;
        gnum = get_user_groups(pw, glist, 10);
        if (setgroups(gnum, glist)) {
          tvhlog(LOG_ALERT, "START",
                 "setgroups() failed, do you have permission?");
          return 1;
        }
      }
      uid     = pw->pw_uid;
      homedir = pw->pw_dir;
      setenv("HOME", homedir, 1);
    } else {
      uid = 1;
    }
    if ((getgid() != gid) && setgid(gid)) {
      tvhlog(LOG_ALERT, "START",
             "setgid() failed, do you have permission?");
      return 1;
    }
    if ((getuid() != uid) && setuid(uid)) {
      tvhlog(LOG_ALERT, "START",
             "setuid() failed, do you have permission?");
      return 1;
    }

    if(daemon(0, 0)) {
      exit(2);
    }
    if(pidfile != NULL) {
      fprintf(pidfile, "%d\n", getpid());
      fclose(pidfile);
    }

    /* Make dumpable */
    if (opt_dump) {
#ifdef PLATFORM_LINUX
      if (chdir("/tmp"))
        tvhwarn("START", "failed to change cwd to /tmp");
      prctl(PR_SET_DUMPABLE, 1);
#else
      tvhwarn("START", "Coredumps not implemented on your platform");
#endif
    }

    umask(0);
  }

  tvheadend_running = 1;

  /* Start log thread (must be done post fork) */
  tvhlog_start();

  /* Alter logging */
  if (opt_fork)
    tvhlog_options &= ~TVHLOG_OPT_STDERR;
  if (!isatty(2))
    tvhlog_options &= ~TVHLOG_OPT_DECORATE;
  
  /* Initialise clock */
  pthread_mutex_lock(&global_lock);
  time(&dispatch_clock);

  /* Signal handling */
  sigfillset(&set);
  sigprocmask(SIG_BLOCK, &set, NULL);
  trap_init(argv[0]);

  /* SSL library init */
  OPENSSL_config(NULL);
  SSL_load_error_strings();
  SSL_library_init();
  
  /* Initialise configuration */
  uuid_init();
  idnode_init();
  config_init(opt_config, opt_nobackup == 0);

  /**
   * Initialize subsystems
   */

  dbus_server_init(opt_dbus, opt_dbus_session);

  intlconv_init();
  
  api_init();

  fsmonitor_init();

#if ENABLE_LIBAV
  libav_init();
  transcoding_init();
#endif

  imagecache_init();

  http_client_init(opt_user_agent);
  esfilter_init();

  service_init();

#if ENABLE_MPEGTS
  mpegts_init(adapter_mask, &opt_satip_xml, &opt_tsfile, opt_tsfile_tuner);
#endif

  channel_init();

  subscription_init();

  dvr_config_init();

  access_init(opt_firstrun, opt_noacl);

#if ENABLE_TIMESHIFT
  timeshift_init();
#endif

  tcp_server_init();
  http_server_register();
  webui_init(opt_xspf);
#if ENABLE_UPNP
  upnp_server_init(opt_bindaddr);
#endif

  service_mapper_init();

  descrambler_init();

  epggrab_init();
  epg_init();

  dvr_init();

  dbus_server_start();

  htsp_register();


  if(opt_subscribe != NULL)
    subscription_dummy_join(opt_subscribe, 1);

  avahi_init();
  bonjour_init();

  epg_updated(); // cleanup now all prev ref's should have been created

  pthread_mutex_unlock(&global_lock);

  /**
   * Wait for SIGTERM / SIGINT, but only in this thread
   */

  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGINT);

  signal(SIGTERM, doexit);
  signal(SIGINT, doexit);

  pthread_sigmask(SIG_UNBLOCK, &set, NULL);

  tvhlog(LOG_NOTICE, "START", "HTS Tvheadend version %s started, "
         "running as PID:%d UID:%d GID:%d, CWD:%s CNF:%s",
         tvheadend_version,
         getpid(), getuid(), getgid(), getcwd(buf, sizeof(buf)),
         hts_settings_get_root());

  if(opt_abort)
    abort();

  mainloop();

#if ENABLE_DBUS_1
  tvhftrace("main", dbus_server_done);
#endif
#if ENABLE_UPNP
  tvhftrace("main", upnp_server_done);
#endif
  tvhftrace("main", htsp_done);
  tvhftrace("main", http_server_done);
  tvhftrace("main", webui_done);
  tvhftrace("main", fsmonitor_done);
#if ENABLE_MPEGTS
  tvhftrace("main", mpegts_done);
#endif
  tvhftrace("main", http_client_done);

  // Note: the locking is obviously a bit redundant, but without
  //       we need to disable the gtimer_arm call in epg_save()
  pthread_mutex_lock(&global_lock);
  tvhftrace("main", epg_save);

#if ENABLE_TIMESHIFT
  tvhftrace("main", timeshift_term);
#endif
  pthread_mutex_unlock(&global_lock);

  tvhftrace("main", epggrab_done);
  tvhftrace("main", tcp_server_done);
  tvhftrace("main", descrambler_done);
  tvhftrace("main", service_mapper_done);
  tvhftrace("main", service_done);
  tvhftrace("main", channel_done);
  tvhftrace("main", dvr_done);
  tvhftrace("main", subscription_done);
  tvhftrace("main", access_done);
  tvhftrace("main", epg_done);
  tvhftrace("main", avahi_done);
  tvhftrace("main", bonjour_done);
  tvhftrace("main", imagecache_done);
  tvhftrace("main", lang_code_done);
  tvhftrace("main", api_done);
  tvhftrace("main", config_done);
  tvhftrace("main", hts_settings_done);
  tvhftrace("main", dvb_done);
  tvhftrace("main", lang_str_done);
  tvhftrace("main", esfilter_done);
  tvhftrace("main", intlconv_done);
  tvhftrace("main", urlparse_done);
  tvhftrace("main", idnode_done);

  tvhlog(LOG_NOTICE, "STOP", "Exiting HTS Tvheadend");
  tvhlog_end();

  if(opt_fork)
    unlink(opt_pidpath);
    
  free(opt_tsfile.str);
  free(opt_satip_xml.str);

  /* OpenSSL - welcome to the "cleanup" hell */
  ENGINE_cleanup();
  RAND_cleanup();
  CRYPTO_cleanup_all_ex_data();
  EVP_cleanup();
  CONF_modules_free();
  COMP_zlib_cleanup();
  ERR_remove_state(0);
  ERR_free_strings();
  {
    sk_SSL_COMP_free(SSL_COMP_get_compression_methods());
  }
  /* end of OpenSSL cleanup code */

#if ENABLE_DBUS_1
  extern void dbus_shutdown(void);
  dbus_shutdown();
#endif
  return 0;
}

/**
 *
 */
void
tvh_str_set(char **strp, const char *src)
{
  free(*strp);
  *strp = src ? strdup(src) : NULL;
}


/**
 *
 */
int
tvh_str_update(char **strp, const char *src)
{
  if(src == NULL)
    return 0;
  free(*strp);
  *strp = strdup(src);
  return 1;
}


/**
 *
 */
void
scopedunlock(pthread_mutex_t **mtxp)
{
  pthread_mutex_unlock(*mtxp);
}


/**
 *
 */  
const char *
hostconnection2str(int type)
{
  switch(type) {
  case HOSTCONNECTION_USB12:
    return "USB (12 Mbit/s)";
    
  case HOSTCONNECTION_USB480:
    return "USB (480 Mbit/s)";

  case HOSTCONNECTION_PCI:
    return "PCI";
  }
  return "Unknown";

}


/**
 *
 */
static int
readlinefromfile(const char *path, char *buf, size_t buflen)
{
  int fd = open(path, O_RDONLY);
  ssize_t r;

  if(fd == -1)
    return -1;

  r = read(fd, buf, buflen - 1);
  close(fd);
  if(r < 0)
    return -1;

  buf[buflen - 1] = 0;
  return 0;
}


/**
 *
 */  
int
get_device_connection(const char *dev)
{
  char path[200];
  char l[64];
  int speed;

  snprintf(path, sizeof(path),  "/sys/class/%s/device/speed", dev);

  if(readlinefromfile(path, l, sizeof(l))) {
    // Unable to read speed, assume it's PCI
    return HOSTCONNECTION_PCI;
  } else {
    speed = atoi(l);
   
    return speed >= 480 ? HOSTCONNECTION_USB480 : HOSTCONNECTION_USB12;
  }
}


