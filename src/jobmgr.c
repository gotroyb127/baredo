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
reply(int wfd, int r)
{
	if (dowrite(wfd, &r, sizeof r) < 0)
		perrnand(longjmp(jbuf, 1), "write");
}

int
jmrun(FPARS(int, jobsn, rfd, wfd))
{
	uint maxrjs, rjobs, pjobs;
	int r, msg;

#define ret(V) \
	do { \
		r = (V); \
		goto end; \
	} while (0)

	prognm = "redo-jobmgr";

	if (setjmp(jbuf))
		ret(0);

	maxrjs = jobsn > 0 ? jobsn : UINT_MAX;
	rjobs = pjobs = 0;
	while (1) {
		switch (read(rfd, &msg, sizeof msg)) {
		case -1:
			perrnand(ret(0), "read");
		case 0:
			ret(1);
		}
		switch (msg) {
		case HASJAVAIL:
			reply(wfd, rjobs < maxrjs);
			break;
		case NEWJREQ:
			if (rjobs < maxrjs)
				reply(wfd, 1), rjobs++;
			else
				pjobs++;
			break;
		case JOBDONE:
			if (pjobs)
				reply(wfd, 1), pjobs--;
			else if (!rjobs--)
				perrfand(ret(0), "Invalid message: no jobs are running");
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
