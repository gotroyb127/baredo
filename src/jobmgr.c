#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <limits.h>

#include "util.h"
#include "jobmgr.h"

extern const char *prognm;
jmp_buf jbuf;

static void
put(int wfd, int r)
{
	if (dowrite(wfd, &r, sizeof r) < 0)
		perrnand(longjmp(jbuf, 1), "write");
}

int
jmrun(FPARS(int, jobsn, rfd, wfd))
{
	unsigned int maxrjs, rjobs, pjobs;
	int rv, msg;

	if (setjmp(jbuf))
		RET(0);

	maxrjs = jobsn > 0 ? jobsn : UINT_MAX;
	rjobs = pjobs = 0;

	put(wfd, 1);
	while (1) {
		switch (read(rfd, &msg, sizeof msg)) {
		case -1:
			perrnand(RET(0), "read");
		case 0:
			RET(1);
		}
		switch (msg) {
		case JOBNEW:
			if (rjobs < maxrjs) {
				if (++rjobs < maxrjs)
					put(wfd, 1);
			} else
				pjobs++;
			break;
		case JOBDONE:
			if (pjobs)
				put(wfd, 1), pjobs--;
			else if (!rjobs--)
				perrfand(RET(0), "Invalid message: no jobs are running");
			else if (rjobs == maxrjs-1)
				put(wfd, 1);
			break;
		case JOBERR:
		default:
			RET(0);
		}
	}
	RET(1);
befret:
	if (close(rfd) < 0 || close(wfd) < 0)
		perrnand(rv = 0, "close");
	return rv;
}
