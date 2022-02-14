enum { /* messages jobs send to the job manager */
	JOBNEW, /* a job is ready to begin */
	JOBDONE, /* a job has successfully completed */
	JOBERR, /* a job failed, don't run new jobs */
};

int jmrun(FPARS(int, jobsn, rfd, wfd));
