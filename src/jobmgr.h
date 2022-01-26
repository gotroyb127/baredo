enum { /* messages jobs send to the job manager */
	HASJAVAIL, /* ask whether a job is available */
	NEWJREQ, /* request for a new job to run */
	JOBDONE, /* a job has successfully completed */
	JOBERR, /* a job failed, don't run new jobs */
};

int jmrun(FPARS(int, jobsn, rfd, wfd));
