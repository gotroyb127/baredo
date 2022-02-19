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
	int r, msg;

#define ret(V) \
	do { \
		r = (V); \
		goto end; \
	} while (0)

	if (setjmp(jbuf))
		ret(0);

	maxrjs = jobsn > 0 ? jobsn : UINT_MAX;
	rjobs = pjobs = 0;

	put(wfd, 1);
	while (1) {
		switch (read(rfd, &msg, sizeof msg)) {
		case -1:
			perrnand(ret(0), "read");
		case 0:
			ret(1);
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
				perrfand(ret(0), "Invalid message: no jobs are running");
			else if (rjobs == maxrjs-1)
				put(wfd, 1);
			break;
		case JOBERR:
		default:
			ret(0);
		}
	}
	ret(1);
end:
	if (close(rfd) < 0 || close(wfd) < 0)
		perrnand(r = 0, "close");
	return r;
#undef ret
}
