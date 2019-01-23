/*++
/* NAME
/*	msg_logger 3
/* SUMMARY
/*	direct diagnostics to logger service
/* SYNOPSIS
/*	#include <msg_logger.h>
/*
/*	void	msg_logger_init(
/*	const char *progname,
/*	const char *hostname,
/*	const char *unix_path,
/*	void	(*fallback)(const char *))
/*
/*	msg_logger_control(
/*	int	key,...)
/* DESCRIPTION
/*	This module implements support to report msg(3) diagnostics
/*	through a logger daemon, with an optional fallback mechanism.
/*	The log record format is like traditional syslog:
/*
/* .nf
/*	    Mmm dd host progname[pid]: text...
/* .fi
/*
/*	msg_logger_init() arranges that subsequent msg(3) calls
/*	will write to an internal logging service.
/*
/*	Arguments:
/* .IP progname
/*	The program name that is prepended to a log record.
/* .IP hostname
/*	The host name that is prepended to a log record. Only the
/*	first hostname label will be used.
/* .IP unix_path
/*	Pathname of a unix-domain datagram service endpoint. A
/*	typical use case is the pathname of the postlog socket.
/* .IP fallback
/*	Null pointer, or pointer to function that will be called
/*	with a formatted message when the logger service is not
/*	(yet) available. A typical use case is to pass the record
/*	to the logwriter(3) module.
/* .PP
/*	msg_logger_control() makes adjustments to the msg_logger
/*	client. The arguments are a list of macros with zero or
/*	more arguments, terminated with CA_MSG_LOGGER_CTL_END
/*	which has none. The following lists the names and the types
/*	of the corresponding value arguments.
/*
/*	Arguments:
/* .IP CA_MSG_LOGGER_CTL_FALLBACK_ONLY
/*	Disable the logging socket, and use the fallback function
/*	only. This cannot be changed with msg_logger_init().
/* .IP CA_MSG_LOGGER_CTL_FALLBACK(void (*)(const char *))
/*	Override the fallback setting (see above) with the specified
/*	function pointer. This cannot be changed with msg_logger_init().
/* SEE ALSO
/*	msg(3)	diagnostics module
/* BUGS
/*	Output records are truncated to ~2000 characters, because
/*	unlimited logging is a liability.
/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Wietse Venema
/*	Google, Inc.
/*	111 8th Avenue
/*	New York, NY 10011, USA
/*--*/

 /*
  * System libraries.
  */
#include <sys_defs.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

 /*
  * Application-specific.
  */
#include <connect.h>
#include <logwriter.h>
#include <msg.h>
#include <msg_logger.h>
#include <msg_output.h>
#include <mymalloc.h>
#include <safe.h>
#include <vstream.h>
#include <vstring.h>

 /*
  * Saved state from msg_logger_init().
  */
static char *msg_logger_progname;
static char *msg_logger_hostname;
static char *msg_logger_unix_path;
static void (*msg_logger_fallback_fn) (const char *);

 /*
  * Saved state from msg_logger_control().
  */
static int msg_logger_fallback_only_override = 0;
static int msg_logger_fallback_fn_override = 0;

 /*
  * Other state.
  */
#define MSG_LOGGER_SOCK_NONE	(-1)

static VSTRING *msg_logger_buf;
static int msg_logger_sock = MSG_LOGGER_SOCK_NONE;

 /*
  * Safety limit.
  */
#define MSG_LOGGER_RECLEN	2000

 /*
  * SLMs.
  */
#define STR(x)	vstring_str(x)
#define LEN(x)	VSTRING_LEN(x)


/* msg_logger_print - log info to service or file */

static void msg_logger_print(int level, const char *text)
{
    time_t  now;
    struct tm *lt;
    ssize_t len;

    /*
     * TODO: this should be a reusable NAME_CODE table plus lookup function.
     */
    static int log_level[] = {
	MSG_INFO, MSG_WARN, MSG_ERROR, MSG_FATAL, MSG_PANIC,
    };
    static char *severity_name[] = {
	"info", "warning", "error", "fatal", "panic",
    };

    /*
     * Format the time stamp.
     */
    if (time(&now) < 0)
	msg_fatal("no time: %m");
    lt = localtime(&now);
    VSTRING_RESET(msg_logger_buf);
    if ((len = strftime(vstring_str(msg_logger_buf),
			vstring_avail(msg_logger_buf),
			"%b %d %H:%M:%S ", lt)) == 0)
	msg_fatal("strftime: %m");
    vstring_set_payload_size(msg_logger_buf, len);

    /*
     * Format the host name (first name label only).
     */
    vstring_sprintf_append(msg_logger_buf, "%.*s ",
			   (int) strcspn(msg_logger_hostname, "."),
			   msg_logger_hostname);

    /*
     * Format the message.
     */
    if (level < 0 || level >= (int) (sizeof(log_level) / sizeof(log_level[0])))
	msg_panic("msg_logger_print: invalid severity level: %d", level);

    if (level == MSG_INFO) {
	vstring_sprintf_append(msg_logger_buf, "%s[%ld]: %.*s",
			       msg_logger_progname, (long) getpid(),
			       (int) MSG_LOGGER_RECLEN, text);
    } else {
	vstring_sprintf_append(msg_logger_buf, "%s[%ld]: %s: %.*s",
			       msg_logger_progname, (long) getpid(),
		       severity_name[level], (int) MSG_LOGGER_RECLEN, text);
    }

    /*
     * Connect to logging service, or fall back to direct log. Many systems
     * will report ENOENT if the endpoint does not exist, ECONNREFUSED if no
     * server has opened the endpoint.
     */
    if (msg_logger_fallback_only_override == 0
	&& msg_logger_sock == MSG_LOGGER_SOCK_NONE) {
	msg_logger_sock = unix_dgram_connect(msg_logger_unix_path, BLOCKING);
	if (msg_logger_sock >= 0)
	    close_on_exec(msg_logger_sock, CLOSE_ON_EXEC);
    }
    /* TODO: a clean way to disable syslog logging. */
    if (msg_logger_sock != MSG_LOGGER_SOCK_NONE) {
	send(msg_logger_sock, STR(msg_logger_buf), LEN(msg_logger_buf), 0);
    } else if (msg_logger_fallback_fn) {
	msg_logger_fallback_fn(STR(msg_logger_buf));
    }
}

/* msg_logger_init - initialize */

void    msg_logger_init(const char *progname, const char *hostname,
	             const char *unix_path, void (*fallback) (const char *))
{
    static int first_call = 1;
    extern char **environ;

    /*
     * XXX If this program is set-gid, then TZ must not be trusted. This
     * scrubbing code is in the wrong place.
     */
    if (unsafe())
	while (getenv("TZ"))			/* There may be multiple. */
	    if (unsetenv("TZ") < 0) {		/* Desperate measures. */
		environ[0] = 0;
		msg_fatal("unsetenv: %m");
	    }
    tzset();

    /*
     * Save the request info. Use free-after-update because this data will be
     * accessed when mystrdup() runs out of memory.
     */
#define FREE_AND_UPDATE(dst, src) do { \
	char *_bak = (dst); \
	(dst) = mystrdup(src); \
	if ((_bak)) myfree(_bak); \
    } while (0)

    FREE_AND_UPDATE(msg_logger_progname, progname);
    FREE_AND_UPDATE(msg_logger_hostname, hostname);
    FREE_AND_UPDATE(msg_logger_unix_path, unix_path);
    if (msg_logger_fallback_fn_override == 0)
	msg_logger_fallback_fn = fallback;

    /*
     * One-time activity: register the output handler, and allocate a buffer.
     */
    if (first_call) {
	first_call = 0;
	msg_output(msg_logger_print);
	msg_logger_buf = vstring_alloc(2048);
    }
}

/* msg_logger_control - tweak the client */

void    msg_logger_control(int name,...)
{
    const char *myname = "msg_logger_control";
    va_list ap;

    /*
     * We use one-way overrides, because a reversible implementation would be
     * difficult to verify (i.e. it would have bugs).
     */
    for (va_start(ap, name); name != MSG_LOGGER_CTL_END; name = va_arg(ap, int)) {
	switch (name) {
	case MSG_LOGGER_CTL_FALLBACK_ONLY:
	    msg_logger_fallback_only_override = 1;
	    if (msg_logger_sock != MSG_LOGGER_SOCK_NONE) {
		(void) close(msg_logger_sock);
		msg_logger_sock = MSG_LOGGER_SOCK_NONE;
	    }
	    break;
	case MSG_LOGGER_CTL_FALLBACK_FN:
	    msg_logger_fallback_fn_override = 1;
	    msg_logger_fallback_fn = va_arg(ap, MSG_LOGGER_FALLBACK_FN);
	    break;
	default:
	    msg_panic("%s: bad name %d", myname, name);
	}
    }
    va_end(ap);
}

#ifdef TEST

 /*
  * Proof-of-concept program to test the msg_logger module.
  * 
  * Usage: msg_logger hostname unix_path fallback_path text...
  */
static char *fallback_path;

static void fallback(const char *msg)
{
    if (logwriter_one_shot(fallback_path, msg) != 0)
	msg_fatal("unable to fall back to directly write %s: %m",
		  fallback_path);
}

int     main(int argc, char **argv)
{
    VSTRING *vp = vstring_alloc(256);

    if (argc < 4)
	msg_fatal("usage: %s host port path text to log", argv[0]);
    msg_logger_init(argv[0], argv[1], argv[2], fallback);
    fallback_path = argv[3];
    argc -= 3;
    argv += 3;
    while (--argc && *++argv) {
	vstring_strcat(vp, *argv);
	if (argv[1])
	    vstring_strcat(vp, " ");
    }
    msg_warn("static text");
    msg_warn("dynamic text: >%s<", vstring_str(vp));
    msg_warn("dynamic numeric: >%d<", 42);
    msg_warn("error text: >%m<");
    msg_warn("dynamic: >%s<: error: >%m<", vstring_str(vp));
    vstring_free(vp);
    return (0);
}

#endif
