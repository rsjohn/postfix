/*++
/* NAME
/*	rand_sleep 3
/* SUMMARY
/*	sleep for randomized interval
/* SYNOPSIS
/*	#include <iostuff.h>
/*
/*	void	rand_sleep(delay, variation)
/*	unsigned delay;
/*	unsigned variation;
/* DESCRIPTION
/*	rand_sleep() blocks the current process for a pseudo-random
/*	amount of time.
/*
/*	Arguments:
/* .IP delay
/*	Time to sleep in microseconds.
/* .IP variation
/*	Sleep time variation in microseconds; must be smaller than
/*	the time to sleep.
/* DIAGNOSTICS
/*	Panic: interface violation. All system call errors are fatal.
/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Wietse Venema
/*	IBM T.J. Watson Research
/*	P.O. Box 704
/*	Yorktown Heights, NY 10598, USA
/*--*/

/* System library. */

#include <sys_defs.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#ifndef RAND_MAX
#define RAND_MAX 0x7fffffff
#endif

/* Utility library. */

#include <msg.h>
#include <iostuff.h>

/* rand_sleep - block for random time */

void    rand_sleep(unsigned delay, unsigned variation)
{
    char   *myname = "rand_sleep";
    static pid_t my_pid;
    unsigned usec;

    /*
     * Sanity checks.
     */
    if (delay == 0)
	msg_panic("%s: bad delay %d", myname, delay);
    if (variation >= delay)
	msg_panic("%s: bad variation %d", myname, variation);

    /*
     * Use the semi-crappy random number generator.
     */
    if (my_pid == 0)
	srandom(my_pid = getpid() ^ time((time_t *) 0));
    usec = (delay - variation / 2) + variation * (double) random() / RAND_MAX;
#ifdef MISSING_USLEEP
    if (usleep(usec) < 0)
	msg_fatal("usleep: %m");
#else
    doze(usec);
#endif
}

#ifdef TEST

#include <msg_vstream.h>

int     main(int argc, char **argv)
{
    int     delay;
    int     variation;

    msg_vstream_init(argv[0], VSTREAM_ERR);
    if (argc != 3)
	msg_fatal("usage: %s delay variation", argv[0]);
    if ((delay = atoi(argv[1])) <= 0)
	msg_fatal("bad delay: %s", argv[1]);
    if ((variation = atoi(argv[2])) < 0)
	msg_fatal("bad variation: %s", argv[2]);
    rand_sleep(delay * 1000000, variation * 1000000);
    exit(0);
}

#endif
