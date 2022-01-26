#define ARGBEGIN \
	for (argc--, argv++; argc > 0; argc--, argv++) { \
		if (argv[0][0] != '-' || !argv[0][1]) \
			break; \
		if (argv[0][1] == '-') { \
			argc--, argv++; \
			break; \
		} \
		int brk_; \
		char *f_; \
		for (brk_ = 0, f_ = *argv+1; *f_ && !brk_; f_++) { \
			switch (*f_)
#define ARGC() (*f_)
#define ARGF() \
	(f_[1] ? brk_ = 1, f_+1 : \
		argc > 1 ? argc--, *argv++ : \
			0)
#define ARGEND \
		} \
	}
