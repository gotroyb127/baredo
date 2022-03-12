/* util.h */

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

#if DBG
	#define perrpref()\
		eprintf("%s: %s[%d]:%s: ", prognm, __FILE__, __LINE__, __func__)
	#define dbgf perrf
#else
	#define dbgf(...) ;
	#define perrpref()\
		eprintf("%s: ", prognm)
#endif

#define perrn(...)\
	do {\
		int errnsv_ = errno;\
		perrpref();\
		eprintf(__VA_ARGS__);\
		errno = errnsv_;\
		perror("");\
	} while (0)
#define perrf(...)\
	do {\
		perrpref();\
		eprintf(__VA_ARGS__);\
		eprintf("\n");\
	} while (0)

#define perrnand(DOWHAT, ...)\
	do {\
		perrn(__VA_ARGS__);\
		DOWHAT;\
	} while (0)
#define perrfand(DOWHAT, ...)\
	do {\
		perrf(__VA_ARGS__);\
		DOWHAT;\
	} while (0)

#define ferrn(...)\
	perrnand(exit(1), __VA_ARGS__)
#define ferrf(...)\
	perrfand(exit(1), __VA_ARGS__)

#define RET(V)\
	do {\
		rv = (V);\
		goto befret;\
	} while (0)

#define intern static

#define PPCAT(A, B) A##B
#define PPECAT(A, B) PPCAT(A, B)
#define PPARG8(_0, _1, _2, _3, _4, _5, _6, _7, _8, ...) _8
#define PPNARGS(...) PPARG8(__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1)

#define FPARS(T, ...)\
	PPECAT(FPARS, PPNARGS(__VA_ARGS__))(T, __VA_ARGS__)

#define FPARS2(T, A, B)\
	T A, T B
#define FPARS3(T, A, B, C)\
	T A, T B, T C
#define FPARS4(T, A, B, C, D)\
	T A, T B, T C, T D
#define FPARS5(T, A, B, C, D, E)\
	T A, T B, T C, T D, T E
#define FPARS6(T, A, B, C, D, E, F)\
	T A, T B, T C, T D, T E, T F
#define FPARS7(T, A, B, C, D, E, F, G)\
	T A, T B, T C, T D, T E, T F, T G
#define FPARS8(T, A, B, C, D, E, F, G, H)\
	T A, T B, T C, T D, T E, T F, T G, T H

ssize_t dowrite(int fd, const void *buf, size_t n);
ssize_t doread(int fd, void *buf, size_t n);
size_t strlcpy(char *dst, const char *src, size_t n);
/* return a pointer to the first path component of a that b doesn't have */
const char *pthpcmp(FPARS(const char, *a, *b));
