#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "util.h"
#include "jobmgr.h"
#include "arg.h"

/* max number of chars added to valid paths as suffix */
#define PTHMAXSUF  (sizeof redir + NAME_MAX)
#define TSEQ(A, B) ((A).tv_sec == (B).tv_sec && (A).tv_nsec == (B).tv_nsec)
#define DIRFROMPATH(D, PATH, CODE) \
	do { \
		char *s_, *D; \
		if (!(s_ = strrchr((PATH), '/'))) \
			D = "."; \
		else if (s_ > (PATH)) \
			D = (PATH), *s_ = '\0'; \
		else \
			D = "/", s_ = NULL; \
		CODE \
		if (s_) \
			*s_ = '/'; \
	} while (0)

/* possible outcomes of executing a .do file */
enum { DOFERR, TRGPHONY, TRGOK };

/* possible outcomes of trying to acquire an exexution lock */
enum { LCKERR, DEPCYCL, LCKREL, LCKACQ };

typedef int (*RedoFn)(char *, int, int);

struct dofile {
	char pth[PATH_MAX];
	char arg1[PATH_MAX];
	char arg2[PATH_MAX];
	char arg3[PATH_MAX];
	char fd1f[PATH_MAX];
};

struct dep {
	int type;
	ino_t ino;
	struct timespec mtim;
	char fnm[PATH_MAX];
};

struct {
	pid_t pid, toppid;
	mode_t dmode, fmode;
	int lvl;
	int pdepfd; /* fd in which parents expects dependency reporting */
	int fsync;
	char topwd[PATH_MAX];
	char wd[PATH_MAX];
	char tmpffmt[PATH_MAX];
	int jmrfd, jmwfd;
} prog;

struct {
	const char *lvl;
	const char *topwd, *toppid;
	const char *pdepfd;
	const char *jmrfd, *jmwfd;
	const char *fsync;
} enm = { /* environment variables names */
	.lvl    = "_REDO_LEVEL",
	.topwd  = "_REDO_TOPWD",
	.toppid = "_REDO_TOPPID",
	.pdepfd = "_REDO_DEPFD",
	.jmrfd  = "_REDO_JMRFD",
	.jmwfd  = "_REDO_JMWFD",
	.fsync  = "REDO_FSYNC",
};

const char *prognm;
const char shell[]      = "/bin/sh";
const char shellflags[] = "-e";
const char redir[]      = ".redo"; /* directory redo expects to use exclusively */

/* function declerations */
static intmax_t strtoint(const char *str, FPARS(intmax_t, min, max, def));
static intmax_t envgeti(const char *nm, FPARS(intmax_t, min, max, def));
static int envgetfd(const char *nm);
static int envsets(FPARS(const char, *nm, *val));
static int envseti(const char *nm, intmax_t n);
static char *normpath(char *abs, size_t n, FPARS(const char, *path, *relto));
static char *relpath(char *rlp, size_t n, FPARS(const char, *path, *relto));
static int mkpath(char *path, mode_t mode);
static int filelck(FPARS(int, fd, cmd, type), FPARS(off_t, start, len));
static int dirsync(const char *dpth);
static int dofisok(const char *pth, int depfd);
static int finddof(const char *trg, struct dofile *df, int depfd);
static int execdof(struct dofile *fd, FPARS(int, lvl, depfd));
static char *redirentry(char *fnm, FPARS(const char, *trg, *suf));
static char *getlckfnm(char *fnm, const char *trg);
static char *getbifnm(char *fnm, const char *trg);
static int repdep(int depfd, char t, const char *trg);
static int fputdep(FILE *f, int t, FPARS(const char, *fnm, *trg));
static int recdeps(FPARS(const char, *bifnm, *rdfnm, *trg));
static int fgetdep(FILE *f, struct dep *dep);
static int depchanged(struct dep *dep, int tdirfd);
static void prstatln(FPARS(int, ok, lvl), FPARS(const char, *trg, *dfpth));
static int acqexlck(int *fd, const char *lckfnm);
static int redo(char *trg, FPARS(int, lvl, pdepfd));
static int redoifchange(char *trg, FPARS(int, lvl, pdepfd));
static int redoifcreate(char *trg, FPARS(int, lvl, pdepfd));
static int redoinfofor(char *trg, FPARS(int, lvl, pdepfd));
static int fredo(RedoFn redofn, char *targ);
static void jredo(RedoFn, char *trg, FPARS(int, *paral, last));
static void vredo(RedoFn redofn, char *trgv[]);
static void vjredo(RedoFn redofn, char *trgv[]);
static void spawnjm(int jobsn);
static int setup(int jobsn);
static void usage(void);

/* function implementations */
intmax_t
strtoint(const char *str, FPARS(intmax_t, min, max, def))
{
	intmax_t i;
	char *e;

	errno = 0;
	i = strtoimax(str, &e, 10);
	if ((*str && *e) || i < min || i > max || errno == ERANGE)
		return def;
	return i;
}

intmax_t
envgeti(const char *nm, FPARS(intmax_t, min, max, def))
{
	char *v;

	if (!(v = getenv(nm)))
		return def;
	return strtoint(v, min, max, def);
}

int
envgetfd(const char *nm)
{
	return envgeti(nm, 0, INT_MAX, -1);
}

int
envsets(FPARS(const char, *nm, *val))
{
	return setenv(nm, val, 1);
}

int
envseti(const char *nm, intmax_t n)
{
	char s[32];

	if (snprintf(s, sizeof s, "%jd", n) >= sizeof s) {
		errno = ERANGE;
		return -1;
	}
	return envsets(nm, s);
}

/* convert path to a normalized absolute path in abs,
   with no ., .. components, and double slashes. if path is not absolute,
   consider it as relative to `relto`, which must be normalized path,
   like /this/nice/abs/path
   return NULL if the nul-terminated result wouldn't fit in n chars */
char *
normpath(char *abs, size_t n, FPARS(const char, *path, *relto))
{
	const char *s;
	char *d;

	d = abs, s = path;
	if (*s != '/') {
		if (n <= strlen(relto))
			return NULL;
		n -= strlen(relto);
		d = stpcpy(abs, relto);
	}
	if (d == abs || d[-1] != '/') {
		if (!n--)
			return NULL;
		*d++ = '/';
	}
	while (*s) {
		while (*s == '/') s++;
		if (*s == '.') {
			if (!s[1])
				break;
			if (s[1] == '/') {
				s += 2;
				continue;
			}
			if (s[1] == '.' && (!s[2] || s[2] == '/')) {
				if (d > abs + 1) /* abs is not "/" */
					for (d--; d[-1] != '/'; d--);
				if (!s[2])
					break;
				s += 3;
				continue;
			}
		}
		while (*s) {
			if (!n--)
				return NULL;
			if ((*d++ = *s++) == '/')
				break;
		}
	}
	while (d > abs + 1 && d[-1] == '/') d--;
	*d = '\0';

	/* ensure that abs last component fits in NAME_MAX chars */
	for (s = d; s[-1] != '/'; s--);
	if (d - s > NAME_MAX)
		return NULL;
	return abs;
}

/* path, relto normalized absolute paths
   relto is a directory */
char *
relpath(char *rlp, size_t n, FPARS(const char, *path, *relto))
{
	const char *p, *r;
	char *d;

	d = rlp;
	p = pthpcmp(path, relto);
	if (*(r = relto + (p - path) - 1))
		do {
			if (n <= 3)
				return NULL;
			n -= 3;
			d = stpcpy(d, "../");
		} while ((r = strchr(r+1, '/')));
	if (strlcpy(d, p, n) >= n)
		return NULL;
	return rlp;
}

/* assuming path is normalized, like this/nice/dir/path
   path is modified but restored */
int
mkpath(char *path, mode_t mode)
{
	struct stat st;
	char *s;

	if (!stat(path, &st)) {
		if (S_ISDIR(st.st_mode))
			return 0;
		errno = ENOTDIR;
		return -1;
	}
	if (errno != ENOENT)
		return -1;

	errno = EEXIST;
	for (s = path; (s = strchr(s+1, '/'));) {
		*s = '\0';
		mkdir(path, mode);
		*s = '/';
		if (errno != EEXIST)
			return -1;
	}
	return mkdir(path, mode);
}

int
filelck(FPARS(int, fd, cmd, type), FPARS(off_t, start, len))
{
	struct flock fl = {
		.l_type = type,
		.l_whence = SEEK_SET,
		.l_start = start,
		.l_len = len,
	};
	return fcntl(fd, cmd, &fl);
}

int
dirsync(const char *dpth)
{
	int dirfd;

	if ((dirfd = open(dpth, O_RDONLY|O_DIRECTORY)) < 0 ||
	fsync(dirfd) < 0 || close(dirfd) < 0)
		return -1;
	return 0;
}

int
dofisok(const char *pth, int depfd)
{
	int e;

	e = !access(pth, F_OK);
	repdep(depfd, e ? '=' : '-', pth);

	return e;
}

/* trg is given as normalized absolute path
   df->arg1 and df->arg2 are also set well */
int
finddof(const char *trg, struct dofile *df, int depfd)
{
	size_t i;
	char *e, *sf, *s; /* end, suffix, suffix */

#define ckdof(DF, A1, SUFLEN) \
	do { \
		if (dofisok((DF)->pth, depfd)) { \
			strcpy((DF)->arg1, (A1)); \
			stpcpy((DF)->arg2, (DF)->arg1)[-SUFLEN] = '\0'; \
			return 1; \
		} \
	} while (0)

	strcpy((e = stpcpy(df->pth, trg)), ".do");
	while (e[-1] != '/') e--;
	i = e - df->pth;
	ckdof(df, trg+i, 0);

	while (i > 0) {
		sf = stpcpy(df->pth+i, "default");
		for (s = strchr(trg+i, '.'); s; s = strchr(s+1, '.')) {
			strcpy(stpcpy(sf, s), ".do");
			ckdof(df, trg+i, strlen(s));
		}
		strcpy(sf, ".do");
		ckdof(df, trg+i, 0);

		while (--i > 0 && trg[i-1] != '/');
	}

	return 0;
#undef ckdof
}

int
execdof(struct dofile *df, FPARS(int, lvl, depfd))
{
	struct stat st, pst;
	pid_t pid;
	int r, ws, fd1, a3fd;
	int unlarg3, unlfd1f;
	char *trg;

#define ret(V) \
	do { \
		r = (V); \
		goto end; \
	} while (0)

	fd1 = a3fd = -1, unlarg3 = unlfd1f = 0;

	sprintf(df->fd1f, "%s.redo.XXXXXX", df->arg1);
	if ((fd1 = mkstemp(df->fd1f)) < 0)
		perrnand(ret(DOFERR), "mkstemp: '%s'", df->fd1f);
	unlfd1f = 1;
	if (fchmod(fd1, prog.fmode) < 0) /* respect umask */
		perrnand(ret(DOFERR), "fchmod: '%s'", df->fd1f);

	sprintf(df->arg3, "%s.%d", df->fd1f, prog.pid);
	if (!access(df->arg3, F_OK))
		perrfand(ret(DOFERR), "assertion failed: '%s' exists",
			df->arg3);

	/* get $1's status to later assert that it has not changed */
	if (stat(df->arg1, &pst) < 0) {
		if (errno != ENOENT)
			perrnand(ret(DOFERR), "stat: '%s'", df->arg1);
		pst.st_size = -1;
	} else
		pst.st_size = 0;

	if ((pid = fork()) < 0)
		perrnand(ret(DOFERR), "fork");
	if (pid == 0) {
		if (envseti(enm.pdepfd, depfd) < 0 ||
		envseti(enm.lvl, lvl) < 0)
			ferrn("envseti");

		if (dup2(fd1, STDOUT_FILENO) < 0)
			ferrn("dup2");
		if (!access(df->pth, X_OK)) {
			execl(df->pth, df->pth, df->arg1, df->arg2, df->arg3,
				(char *)0);
			ferrn("execl");
		}
		execl(shell, shell, shellflags, df->pth, df->arg1, df->arg2,
			df->arg3, (char *)0);
		ferrn("execl");
	}
	wait(&ws);
	if (!WIFEXITED(ws) || WEXITSTATUS(ws) != 0)
		ret(DOFERR);
	unlarg3 = 1;

	/* assert that $1 hasn't changed */
	if (stat(df->arg1, &st) < 0) {
		if (errno != ENOENT)
			perrnand(ret(DOFERR), "stat: '%s'", df->arg1);
		if (pst.st_size >= 0)
			perrfand(ret(DOFERR), "aborting: .do file has removed $1");
	} else {
		if (pst.st_size < 0)
			perrfand(ret(DOFERR), "aborting: .do file has created $1");
		if (!TSEQ(pst.st_ctim, st.st_ctim))
			perrfand(ret(DOFERR), "aborting: .do file modified $1");
	}

	/* determine whether $3 or stdout is the target */
	trg = NULL;
	if (!access(df->arg3, F_OK)) { /* .do file created $3 */
		trg = df->arg3, unlarg3 = 0;
		if ((a3fd = open(df->arg3, O_RDONLY)) < 0)
			perrnand(ret(DOFERR), "open: '%s'", df->arg3);
	} else if (errno != ENOENT)
		perrnand(ret(DOFERR), "stat: '%s'", df->arg3);
	if (stat(df->fd1f, &st) < 0)
		perrnand(ret(DOFERR), "stat: '%s'", df->fd1f);
	else if (st.st_size > 0) { /* .do file wrote to stdout */
		if (trg) { /* .do file also created $3 */
			unlarg3 = 1;
			perrf("aborting: .do file created $3 AND wrote to stdout");
			ret(DOFERR);
		}
		trg = df->fd1f, unlfd1f = 0;
	}
	if (!trg)
		ret(TRGPHONY);

	/* fsync the target, rename, fsync target's directory */
	if (prog.fsync) {
		if (trg == df->arg3) {
			if (fsync(a3fd) < 0)
				perrnand(ret(DOFERR), "fsync: '%s'", df->arg3);
		} else if (fsync(fd1) < 0)
			perrnand(ret(DOFERR), "fsync: '%s'", df->fd1f);
	}
	if (rename(trg, df->arg1) < 0)
		perrnand(ret(DOFERR), "rename: '%s' -> '%s'", trg, df->arg1);
	if (prog.fsync)
		DIRFROMPATH(dir, df->arg1,
			if (dirsync(dir) < 0)
				perrnand(ret(DOFERR), "dirsync: '%s'", dir);
		);
	ret(TRGOK);
end:
	if (fd1 >= 0 && close(fd1) < 0)
		perrnand(r = DOFERR, "close: '%s'", df->fd1f);
	if (a3fd >= 0 && close(a3fd) < 0)
		perrnand(r = DOFERR, "close: '%s'", df->arg3);
	if (unlarg3 && unlink(df->arg3) < 0 && errno != ENOENT)
		perrnand(r = DOFERR, "unlink: '%s'", df->arg3);
	if (unlfd1f && unlink(df->fd1f) < 0 && errno != ENOENT)
		perrnand(r = DOFERR, "unlink: '%s'", df->fd1f);
	return r;
#undef ret
}

char *
redirentry(char *fnm, FPARS(const char, *trg, *suf))
{
	char *s;

	s = strrchr(trg, '/');
	sprintf(fnm, "%.*s/%s/%s.%s", (int)(s - trg), trg,
		redir, s+1, suf);
	return fnm;
}

char *
getlckfnm(char *fnm, const char *trg)
{
	return redirentry(fnm, trg, "lck");
}

char *
getbifnm(char *fnm, const char *trg)
{
	return redirentry(fnm, trg, "bi");
}

int
repdep(int depfd, char t, const char *depfnm)
{
	if (dprintf(depfd, "%c%s", t, depfnm) < 0 ||
	dowrite(depfd, "", 1) < 0)
		perrfand(return 0, "write");
	return 1;
}

int
fputdep(FILE *f, int t, FPARS(const char, *fnm, *trg))
{
	struct stat st;
	char rlp[PATH_MAX], tdir[PATH_MAX];

	fputc(t, f);
	if (t != '-') {
		if (stat(fnm, &st))
			perrnand(return 0, "stat: '%s'", fnm);
		fwrite(&st.st_ino, sizeof st.st_ino, 1, f);
		fwrite(&st.st_mtim, sizeof st.st_mtim, 1, f);
	}
	strlcpy(tdir, trg, strrchr(trg, '/') - trg);
	if (!relpath(rlp, sizeof rlp, fnm, tdir))
		perrnand(return 0, "%s", strerror(ENAMETOOLONG));
	fputs(rlp, f);
	fwrite("", 1, 1, f);
	return !ferror(f);
}

int
recdeps(FPARS(const char, *bifnm, *rdfnm, *trg))
{
	FILE *rf, *wf;
	size_t i;
	int rfopen, wfopen;
	int c, r;
	char depln[PATH_MAX+1]; /* type, PATH */
	char wrfnm[PATH_MAX];

#define ret(V) \
	do { \
		r = (V); \
		goto end; \
	} while (0)

	sprintf(wrfnm, "%s.t", bifnm); /* write to a temporary file at first */
	rfopen = wfopen = 0;
	if (!(rf = fopen(rdfnm, "r")))
		perrnand(ret(0), "fopen: '%s'", rdfnm);
	rfopen = 1;
	if (!(wf = fopen(wrfnm, "w")))
		perrnand(ret(0), "fopen: '%s'", wrfnm);
	wfopen = 1;

	if (filelck(fileno(wf), F_SETLKW, F_WRLCK, 0, 0) < 0)
		perrnand(ret(0), "filelck: '%s'", wrfnm);
	if (!fputdep(wf, ':', trg, trg))
		ret(0);
	i = 0;
	while ((c = fgetc(rf)) != EOF) {
		if (!(depln[i++] = c)) {
			if (!fputdep(wf, depln[0], depln+1, trg))
				ret(0);
			i = 0;
			continue;
		}
		if (i >= sizeof depln)
			ret(0);
	}
	if (!feof(rf))
		ret(0);

	/* fsync bifile, rename, fsync directory */
	if (prog.fsync && fsync(fileno(wf)) < 0)
		perrnand(ret(0), "fsync: '%s'", wrfnm);
	if (rename(wrfnm, bifnm) < 0)
		perrnand(ret(0), "rename: '%s' -> '%s'", wrfnm, bifnm);
	if (prog.fsync)
		DIRFROMPATH(dir, wrfnm,
			if (dirsync(dir) < 0)
				perrnand(ret(0), "dirsync: '%s'", dir);
		);
	ret(1);
end:
	if (rfopen && fclose(rf) == EOF)
		perrnand(r = 0, "fclose: '%s'", rdfnm);
	if (wfopen && fclose(wf) == EOF)
		perrnand(r = 0, "fclose: '%s'", wrfnm);
	return r;
#undef ret
}

/* return 0 when file is invalid */
int
fgetdep(FILE *f, struct dep *dep)
{
	size_t i;
	int t, c;

	switch (t = fgetc(f)) {
	case ':':
	case '=':
	case '-':
		break;
	default:
		return 0;
	}
	if ((dep->type = t) != '-')
		if (fread(&dep->ino, sizeof dep->ino, 1, f) != 1 ||
		fread(&dep->mtim, sizeof dep->mtim, 1, f) != 1)
			return 0;
	i = 0;
	while ((c = fgetc(f)) != EOF) {
		if (!(dep->fnm[i++] = c))
			break;
		if (i >= sizeof dep->fnm)
			return 0;
	}
	if (!i || ferror(f))
		return 0;
	return 1;
}

int
depchanged(struct dep *dep, int tdirfd)
{
	struct stat st;

	switch (dep->type) {
	case ':':
	case '=':
		if (!fstatat(tdirfd, dep->fnm, &st, 0) &&
		dep->ino == st.st_ino && TSEQ(dep->mtim, st.st_mtim))
			return 0;
	case '-':
		if (faccessat(tdirfd, dep->fnm, F_OK, 0))
			return 0;
	}
	return 1;
}

void
prstatln(FPARS(int, ok, lvl), FPARS(const char, *trg, *dfpth))
{
	char rlp[PATH_MAX];
	int i;

	if (filelck(STDERR_FILENO, F_SETLKW, F_WRLCK, 0, 0) < 0)
		perrnand(return, "filelck: /dev/stderr");

	eprintf("redo %s ", ok ? "ok" : "err");
	for (i = 0; i < lvl; i++)
		eprintf(". ");
	eprintf("%s", relpath(rlp, sizeof rlp, trg, prog.topwd) ? rlp : trg);
	eprintf(" (%s)\n", relpath(rlp, sizeof rlp, dfpth, prog.topwd) ?
		rlp : dfpth);

	if (filelck(STDERR_FILENO, F_SETLK, F_UNLCK, 0, 0) < 0)
		perrnand(return, "filelck: /dev/stderr");
}

/* acquire an execution lock
   if a lockfile exists then either
   . the file has an active lock (the proccess that has the lock is running)
     which is hold by a redo proccess trying to build the same target, so either
   . . there is a dependency cycle <=> the pid stored in the file equals prog.pid
   . . or another independent redo is building the target
   . no lock is active, so the proccess that created the lockfile was killed
     and the pid stored in that cannot be trusted
 */
int
acqexlck(int *fd, const char *lckfnm)
{
	pid_t pid;
	int lckfd, r;

#define ret(V) \
	do { \
		r = (V); \
		goto end; \
	} while (0)

	if ((lckfd = open(lckfnm, O_RDWR|O_CREAT|O_CLOEXEC, prog.fmode)) < 0)
		perrnand(ret(LCKERR), "open: '%s", lckfnm);
	if (filelck(lckfd, F_SETLK, F_WRLCK, 0, 2) < 0) {
		if (errno != EAGAIN && errno != EACCES)
			perrnand(ret(LCKERR), "filelck: '%s'", lckfnm);
		/* lock is held by another proccess, wait until it is safe
		   to read the pid */
		if (filelck(lckfd, F_SETLKW, F_RDLCK, 1, 1) < 0)
			perrnand(ret(LCKERR), "filelck: '%s'", lckfnm);
		if (doread(lckfd, &pid, sizeof(pid_t)) < 0)
			perrnand(ret(LCKERR), "read: '%s'", lckfnm);
		if (pid == prog.toppid)
			ret(DEPCYCL);
		/* wait until the write lock gets released */
		if (filelck(lckfd, F_SETLKW, F_WRLCK, 0, 2) < 0)
			perrnand(ret(LCKERR), "filelck: '%s'", lckfnm);
		ret(LCKREL); /* lock has been released */
	}
	if (dowrite(lckfd, &prog.toppid, sizeof(pid_t)) < 0)
		perrnand(ret(LCKERR), "write: '%s'", lckfnm);
	/* notify that it is safe to read the pid */
	if (filelck(lckfd, F_SETLK, F_UNLCK, 1, 1) < 0)
		perrnand(ret(LCKERR), "filelck: '%s'", lckfnm);
	*fd = lckfd;
	return LCKACQ;
end:
	if (lckfd >= 0 && close(lckfd) < 0)
		perrnand(r = LCKERR, "close");
	return r;
#undef ret
}

int
redo(char *trg, FPARS(int, lvl, pdepfd))
{
	struct dofile df;
	int depfd, lckfd;
	int ok, r;
	char tmp[PATH_MAX], lckfnm[PATH_MAX], tmpdepfnm[sizeof prog.tmpffmt];
	char *s;

#define ret(V) \
	do { \
		r = (V); \
		goto end; \
	} while (0)

	lckfd = -1;
	if ((depfd = mkstemp(strcpy(tmpdepfnm, prog.tmpffmt))) < 0)
		perrnand(ret(0), "mkstemp: '%s'", tmpdepfnm);

	if (!finddof(trg, &df, depfd))
		perrfand(ret(0), "no .do file for '%s'", trg);

	/* chdir to the directory the .do file is in */
	DIRFROMPATH(dir, df.pth,
		if (chdir(dir) < 0)
			perrnand(ret(0), "chdir: '%s'", dir);
	);

	/* create required path */
	s = (s = strrchr(df.arg1, '/')) ? s+1 : df.arg1;
	sprintf(tmp, "%.*s%s", (int)(s - df.arg1), df.arg1, redir);
	if (mkpath(tmp, prog.dmode) < 0)
		perrnand(ret(0), "mkpath: '%s'", tmp);

	switch (acqexlck(&lckfd, getlckfnm(lckfnm, trg))) {
	case DEPCYCL:
		perrf("'%s': dependency cycle detected", relpath(tmp,
			sizeof tmp, trg, prog.topwd) ? tmp : trg);
	default:
	case LCKERR:
		lckfd = -1;
		ret(0);
	case LCKREL:
		lckfd = -1;
		ret(redoifchange(trg, lvl, pdepfd));
	case LCKACQ:
		break;
	}

	/* exec */
	ok = execdof(&df, lvl+1, depfd);
	prstatln(ok >= TRGPHONY, lvl, trg, df.pth);

	/* don't record dependencies unless target was successfully created */
	if (ok < TRGOK)
		ret(ok == TRGPHONY);

	if (pdepfd >= 0 && !repdep(pdepfd, '=', trg))
		ret(0);
	if (!recdeps(getbifnm(tmp, trg), tmpdepfnm, trg))
		ret(0);
	ret(1);
end:
	if (depfd >= 0) {
		if (close(depfd) < 0)
			perrnand(r = 0, "close");
		if (unlink(tmpdepfnm) < 0 && errno != ENOENT)
			perrnand(r = 0, "unlink: '%s'", tmpdepfnm);
	}
	if (lckfd >= 0) {
		if (close(lckfd) < 0)
			perrnand(r = 0, "close");
		if (unlink(lckfnm) < 0 && errno != ENOENT)
			perrnand(r = 0, "unlink: '%s'", lckfnm);
	}
	return r;
#undef ret
}

int
redoifchange(char *trg, FPARS(int, lvl, pdepfd))
{
	FILE *bif; /* build info file */
	struct dep dep;
	int tdirfd;
	int clbif, r, c;
	char tdir[PATH_MAX], bifnm[PATH_MAX], depfnm[PATH_MAX];

#define ret(V) \
	do { \
		r = (V); \
		goto end; \
	} while (0)

	clbif = 0, tdirfd = -1;
	/* target doesn't exist */
	if (access(trg, F_OK))
		goto rebuild;

	DIRFROMPATH(dir, trg,
		strcpy(tdir, dir);
	);
	if ((tdirfd = open(tdir, O_RDONLY|O_DIRECTORY|O_CLOEXEC)) < 0)
		perrnand(ret(0), "open: '%s'", tdir);

	/* when trg exists but build info in .redo/ doesn't,
	   assume trg in not supposed to been build by redo */
	if (access(getbifnm(bifnm, trg), F_OK)) {
		if (pdepfd >= 0 && !repdep(pdepfd, '=', trg))
			ret(0);
		ret(1);
	}

	if (!(bif = fopen(bifnm, "r")))
		perrnand(ret(0), "fopen: '%s'", bifnm);
	clbif = 1;
	if (filelck(fileno(bif), F_SETLKW, F_RDLCK, 0, 0) < 0)
		perrnand(ret(0), "filelck: '%s'", bifnm);

	if (!fgetdep(bif, &dep) || dep.type != ':')
		goto rebuild;
	if (depchanged(&dep, tdirfd))
		perrfand(ret(0), "aborting: '%s' was externally modified",
			dep.fnm);
	while ((c = fgetc(bif)) != EOF) {
		ungetc(c, bif);
		if (!fgetdep(bif, &dep))
			goto rebuild;
		if (dep.type == '=') {
			if (!normpath(depfnm, sizeof depfnm - PTHMAXSUF,
			dep.fnm, tdir)) {
				errno = ENAMETOOLONG;
				perrfand(ret(0), "'%s'", dep.fnm);
			}
			if (!redoifchange(depfnm, lvl+1, -1))
				ret(0);
		}
		if (depchanged(&dep, tdirfd))
			goto rebuild;
	}
	if (pdepfd >= 0 && !repdep(pdepfd, '=', trg))
		perrnand(ret(0), "repdep: '%s'", trg);
	ret(1);
rebuild:
	ret(redo(trg, lvl, pdepfd));
end:
	if (tdirfd >= 0 && close(tdirfd) < 0)
		perrnand(r = 0, "close: '%s'", tdir);
	if (clbif && fclose(bif) == EOF)
		perrnand(r = 0, "fclose: '%s'", bifnm);
	return r;
#undef ret
}

int
redoifcreate(char *trg, FPARS(int, lvl, pdepfd))
{
	if (pdepfd < 0)
		perrfand(return 0, "wrong usage: redo what?");
	return repdep(pdepfd, '-', trg);
}

int
redoinfofor(char *trg, FPARS(int, lvl, pdepfd))
{
	FILE *bif;
	struct dep dep;
	int clbif, c, r;
	char bifnm[PATH_MAX];

#define ret(V) \
	do { \
		r = (V); \
		goto end; \
	} while (0)

	clbif = 0;
	if (!(bif = fopen(getbifnm(bifnm, trg), "r"))) {
		if (errno != ENOENT)
			perrnand(ret(0), "fopen: '%s'", bifnm);
		char rlp[PATH_MAX];
		printf("'%s': not build by redo\n",
			relpath(rlp, sizeof rlp, trg, prog.wd) ? rlp : trg);
		ret(0);
	}
	clbif = 1;

	if (filelck(fileno(bif), F_SETLKW, F_RDLCK, 0, 0) < 0)
		perrnand(ret(0), "filelck: '%s'", bifnm);
	do {
		if (!fgetdep(bif, &dep))
			goto invlf;
		printf("%c ", (char)dep.type);
		if (dep.type != '-')
			printf("%ju %jd %jd ", (uintmax_t)dep.ino,
				(intmax_t)dep.mtim.tv_sec,
				(intmax_t)dep.mtim.tv_nsec);
		printf("%s\n", dep.fnm);
		if ((c = fgetc(bif)) == EOF)
			break;
		ungetc(c, bif);
	} while (1);
	if (ferror(bif))
		perrnand(ret(0), "ferror: '%s'", bifnm);
	ret(1);
invlf:
	perrfand(ret(0), "'%s': invalid build-info file", bifnm);
end:
	if (clbif && fclose(bif) == EOF)
		perrnand(r = 0, "fclose: '%s'", bifnm);
	return r;
#undef ret
}

int
fredo(RedoFn redofn, char *targ)
{
	char trg[PATH_MAX];

	if (!normpath(trg, sizeof trg - PTHMAXSUF, targ, prog.wd))
		perrfand(return 0, "'%s': %s", targ, strerror(ENAMETOOLONG));

	return redofn(trg, prog.lvl, prog.pdepfd);
}

void
jredo(RedoFn redofn, char *trg, FPARS(int, *paral, last))
{
	int s, w, r, msg, st;

#define ex(V) \
	do { \
		s = (V); \
		goto end; \
	} while (0)

	w = 0;
	if (!last || *paral) {
		msg = !*paral ? HASJAVAIL : NEWJREQ;
		if (dowrite(prog.jmwfd, &msg, sizeof msg) < 0)
			perrnand(ex(1), "write");

		switch (read(prog.jmrfd, &r, sizeof r)) {
		case -1:
			perrn("read");
		case 0:
			ex(1);
		}
		if (!last && (*paral || r)) {
			switch (fork()) {
			case -1:
				perrnand(ex(1), "fork");
			case 0:
				*paral = 1;
				prog.pid = getpid();
				return;
			default:
				w = 1;
			}
		}
	}

	s = fredo(redofn, trg);
	if (!s || *paral) {
		msg = s ? JOBDONE : JOBERR;
		if (dowrite(prog.jmwfd, &msg, sizeof msg) < 0)
			perrnand(ex(1), "write");
	}
	if (!*paral) {
		if (w)
			ex(!s);
		if (s)
			return;
		ex(1);
	}
	ex(!s);
end:
	if (w) {
		if (wait(&st) < 0)
			perrnand(s = 0, "wait");
		s = s || !WIFEXITED(st) || WEXITSTATUS(st);
	}
	exit(s);
#undef ex
}

void
vredo(RedoFn redofn, char *trgv[])
{
	for (; *trgv; trgv++)
		if (!fredo(redofn, *trgv))
			exit(1);
}

void
vjredo(RedoFn redofn, char *trgv[])
{
	int paral;

	paral = 0;
	for (; *trgv; trgv++)
		jredo(redofn, *trgv, &paral, !trgv[1]);
}

/* spawn job manager, non-jm child returns */
void
spawnjm(int jobsn)
{
	pid_t pid;
	int wp[2], rp[2];
	int st, r;

	if (pipe(wp) < 0 || pipe(rp) < 0)
		ferrn("pipe");
	switch (pid = fork()) {
	case -1:
		ferrn("fork");
	case 0:
		prog.jmwfd = wp[1];
		prog.jmrfd = rp[0];
		if (envseti(enm.jmrfd, prog.jmrfd) < 0 ||
		envseti(enm.jmwfd, prog.jmwfd) < 0)
			ferrn("envseti");
		if (close(wp[0]) < 0 || close(rp[1]) < 0)
			ferrn("close");
		return;
	}
	if (close(wp[1]) < 0 || close(rp[0]) < 0)
		perrnand(goto wt, "close");

	r = jmrun(jobsn, wp[0], rp[1]);
wt:
	if (wait(&st) < 0)
		perrnand(r = 1, "wait");
	exit(!r || !WIFEXITED(st) || WEXITSTATUS(st));
}

int
setup(int jobsn)
{
	int withjm;
	mode_t mask;
	const char *d;
	char *e;
	size_t n;

	withjm = 0;
	if ((prog.jmrfd = envgetfd(enm.jmrfd)) < 0) {
		if (jobsn) {
			spawnjm(jobsn);
			withjm = 1;
		}
	} else {
		withjm = 1;
		if ((prog.jmwfd = envgetfd(enm.jmwfd)) < 0)
			ferrf("invalid environment values for '%s' and '%s'",
				enm.jmrfd, enm.jmwfd);
	}

	umask(mask = umask(0)); /* get and restore umask */
	prog.fmode = (prog.dmode = 0777 & ~mask) & ~0111;
	prog.pid = getpid();

	if (!getcwd(prog.wd, sizeof prog.wd))
		ferrn("getcwd");

	if (!(prog.lvl = envgeti(enm.lvl, 1, INT_MAX, 0))) {
		strcpy(prog.topwd, prog.wd);
		prog.toppid = prog.pid;
		prog.pdepfd = -1;

		if (envsets(enm.topwd, prog.wd) < 0)
			ferrn("envsets");
		if (envseti(enm.toppid, prog.toppid) < 0)
			ferrn("envseti");
	} else {
		if ((prog.toppid = envgeti(enm.toppid, 1, INT_MAX, -1)) < 0)
			ferrf("invalid environment variable '%s'", enm.toppid);
		if ((prog.pdepfd = envgetfd(enm.pdepfd)) < 0)
			ferrf("invalid environment variable '%s'", enm.pdepfd);
		if (!(e = getenv(enm.topwd)))
			ferrf("invalid environment values for '%s' and '%s'",
				enm.lvl, enm.topwd);
		strlcpy(prog.topwd, e, sizeof prog.topwd);
	}
	d = (d = getenv("TMPDIR")) ? d : "/tmp";
	n = sizeof prog.tmpffmt;
	if (snprintf(prog.tmpffmt, n, "%s/redo.tmp.XXXXXX", d) >= n)
		ferrf("$TMPDIR: %s", strerror(ENAMETOOLONG));

	prog.fsync = envgeti(enm.fsync, 0, 1, 1);

	return withjm;
}

void
usage(void)
{
	ferrf("usage: redo [-j njobs] targets...");
}

int
main(int argc, char *argv[])
{
	RedoFn redofn;
	int jobsn;
	char *s;

	prognm = (s = strrchr(argv[0], '/')) ? s+1 : argv[0];

	jobsn = 1;
	ARGBEGIN {
	case 'j':
		if (!(s = ARGF()))
			perrfand(usage(), "missing argument for -j");
		if ((jobsn = strtoint(s, 0, INT_MAX, -1)) < 0)
			perrfand(usage(), "'%s': Invalid number", s);
		break;
	default:
		perrfand(usage(), "-%c: Wrong option", ARGC());
	} ARGEND
	if (!argc)
		perrfand(usage(), "No targets given");

	if (!strcmp(prognm, "redo"))
		redofn = &redo;
	else if (!strcmp(prognm, "redo-ifchange"))
		redofn = &redoifchange;
	else if (!strcmp(prognm, "redo-ifcreate"))
		redofn = &redoifcreate;
	else if (!strcmp(prognm, "redo-infofor"))
		redofn = &redoinfofor;
	else
		ferrf("'%s': not implemented", prognm);

	if (setup(jobsn - 1))
		vjredo(redofn, argv);
	else
		vredo(redofn, argv);

	return 0;
}
