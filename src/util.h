/* util.h */

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

#if DBG
	#define perrpref() \
		eprintf("%s[%d]: ", prognm, __LINE__)
#else
	#define perrpref() \
		eprintf("%s: ", prognm)
#endif

#define perrn(...) \
	do { \
		int errsv_ = errno; \
		perrpref(); \
		eprintf(__VA_ARGS__); \
		eprintf(": %s\n", strerror(errsv_)); \
	} while (0)
#define perrf(...) \
	do { \
		perrpref(); \
		eprintf(__VA_ARGS__); \
		eprintf("\n"); \
	} while (0)

#define perrnand(DOWHAT, ...) \
	do { \
		perrn(__VA_ARGS__); \
		DOWHAT; \
	} while (0)
#define perrfand(DOWHAT, ...) \
	do { \
		perrf(__VA_ARGS__); \
		DOWHAT; \
	} while (0)

#define ferr(...) \
	perrnand(exit(1), __VA_ARGS__)
#define ferrf(...) \
	perrfand(exit(1), __VA_ARGS__)

#define bcase break; case

#define CAT_(A, B) A##B
#define CAT(A, B) CAT_(A, B)
#define ARG_8(_0, _1, _2, _3, _4, _5, _6, _7, _8, ...) _8
#define VA_NARGS(...) ARG_8(__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1)

#define FPARS(type, ...) \
	CAT(FPARS_, VA_NARGS(__VA_ARGS__))(type, __VA_ARGS__)
#define FPARS_2(type, _1, _2) \
	type _1, type _2
#define FPARS_3(type, _1, _2, _3) \
	FPARS_2(type, _1, _2), type _3
#define FPARS_4(type, _1, _2, _3, _4) \
	FPARS_3(type, _1, _2, _3), type _4
#define FPARS_5(type, _1, _2, _3, _4, _5) \
	FPARS_4(type, _1, _2, _3, _4), type _5
#define FPARS_6(type, _1, _2, _3, _4, _5, _6) \
	FPARS_5(type, _1, _2, _3, _4, _5), type _6
#define FPARS_7(type, _1, _2, _3, _4, _5, _6, _7) \
	FPARS_6(type, _1, _2, _3, _4, _5, _6), type _7
#define FPARS_7(type, _1, _2, _3, _4, _5, _6, _7) \
	FPARS_6(type, _1, _2, _3, _4, _5, _6), type _7
#define FPARS_8(type, _1, _2, _3, _4, _5, _6, _7, _8) \
	FPARS_7(type, _1, _2, _3, _4, _5, _6, _7), type _8

ssize_t dowrite(int fd, const void *buf, size_t n);
ssize_t doread(int fd, void *buf, size_t n);
size_t strlcpy(char *dst, const char *src, size_t n);
/* return a pointer to the first path component of a that b doesn't have */
const char *pthpcmp(FPARS(const char, *a, *b));
