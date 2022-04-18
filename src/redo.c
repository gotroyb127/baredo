#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"
#include "jobmgr.h"
#include "arg.h"

/* max number of chars added to valid paths as suffix */
#define PTHMAXSUF  (sizeof redir + NAME_MAX)
#define TSEQ(A, B) ((A).tv_sec == (B).tv_sec && (A).tv_nsec == (B).tv_nsec)
#define DIRFROMPATH(D, PATH, CODE)\
	do {\
		char *s_, *D;\
		if (!(s_ = strrchr((PATH), '/')))\
			D = ".";\
		else if (s_ > (PATH))\
			D = (PATH), *s_ = '\0';\
		else\
			D = "/", s_ = NULL;\
		CODE\
		if (s_)\
			*s_ = '/';\
	} while (0)

/* possible outcomes of executing a .do file */
enum { DOFINT, DOFERR, TRGSAME, TRGNEW };

/* possible outcomes of trying to acquire an exexution lock */
enum { LCKERR, DEPCYCL, LCKREL, LCKACQ };

typedef int redofnt(char *, int, int);

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
	int retonsig; /* whether the signal handler should return instead of exiting */
	int fsync;
	char topwd[PATH_MAX];
	char wd[PATH_MAX];
	char tmpffmt[PATH_MAX];
	int withjm;
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

intern intmax_t strtoint(const char *str, FPARS(intmax_t, min, max, def));
intern intmax_t envgeti(const char *nm, FPARS(intmax_t, min, max, def));
intern int envgetfd(const char *nm);
intern int envsets(FPARS(const char, *nm, *val));
intern int envseti(const char *nm, intmax_t n);
intern char *normpath(char *abs, size_t n, FPARS(const char, *path, *relto));
intern char *relpath(char *rlp, size_t n, FPARS(const char, *path, *relto));
intern int mkpath(char *path, mode_t mode);
intern int filelck(FPARS(int, fd, cmd, type), FPARS(off_t, start, len));
intern int dirsync(const char *dpth);
intern int dofisok(const char *pth, int depfd);
intern int finddof(const char *trg, struct dofile *df, int depfd);
intern int execdof(struct dofile *fd, FPARS(int, lvl, depfd));
intern char *redirentry(char *fnm, FPARS(const char, *trg, *suf));
intern char *getlckfnm(char *fnm, const char *trg);
intern char *getbifnm(char *fnm, const char *trg);
intern int repdep(int depfd, char t, const char *trg);
intern int fputdep(FILE *f, int t, FPARS(const char, *fnm, *trg));
intern int recdeps(FPARS(const char, *bifnm, *rdfnm, *trg));
intern int fgetdep(FILE *f, struct dep *dep);
intern int depchanged(struct dep *dep, int tdirfd);
intern void pstatln(FPARS(int, ok, lvl), FPARS(const char, *trg, *dfpth));
intern int acqexlck(int *fd, const char *lckfnm);
intern int redo(char *trg, FPARS(int, lvl, pdepfd));
intern int redoifchange(char *trg, FPARS(int, lvl, pdepfd));
intern int redoifcreate(char *trg, FPARS(int, lvl, pdepfd));
intern int redoinfofor(char *trg, FPARS(int, lvl, pdepfd));
intern int fredo(redofnt *, char *targ);
intern void jredo(redofnt *, char *trg, FPARS(int, *paral, hnext));
intern void vredo(redofnt *, int trgc, char *trgv[]);
intern void vjredo(redofnt *, int trgc, char *trgv[]);
intern void spawnjm(int jobsn);
intern void onsig(int sig);
intern void setup(int jobsn);
intern void usage(void);

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

#define ckdof(DF, A1, SUFLEN)\
	do {\
		if (dofisok((DF)->pth, depfd)) {\
			strcpy((DF)->arg1, (A1));\
			stpcpy((DF)->arg2, (DF)->arg1)[-SUFLEN] = '\0';\
			return 1;\
		}\
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
	pid_t cld;
	int ws, fd1, a3fd, rv;
	int unlarg3, unlfd1f;
	char *trg;

	fd1 = a3fd = -1, unlarg3 = unlfd1f = 0;

	sprintf(df->fd1f, "%s.redo.XXXXXX", df->arg1);
	if ((fd1 = mkstemp(df->fd1f)) < 0)
		perrnand(RET(DOFERR), "mkstemp: %s", df->fd1f);
	unlfd1f = 1;
	if (fchmod(fd1, prog.fmode) < 0) /* respect umask */
		perrnand(RET(DOFERR), "fchmod: %s", df->fd1f);

	sprintf(df->arg3, "%s.%d", df->fd1f, prog.pid);
	if (!access(df->arg3, F_OK))
		perrfand(RET(DOFERR), "assertion failed: %s exists",
			df->arg3);

	/* get $1's status to later assert that it has not changed */
	if (stat(df->arg1, &pst) < 0) {
		if (errno != ENOENT)
			perrnand(RET(DOFERR), "stat: %s", df->arg1);
		pst.st_size = -1;
	} else
		pst.st_size = 0;

	if ((cld = fork()) < 0)
		perrnand(RET(DOFERR), "fork");
	else if (!cld) {
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

	prog.retonsig = 1;
	if (waitpid(cld, &ws, 0) < 0) {
		if (errno != EINTR)
			perrnand(RET(DOFERR), "waitpid");
		unlfd1f = !fstat(fd1, &st) && !st.st_size;
		RET(DOFINT);
	}
	prog.retonsig = 0;

	unlarg3 = 1;
	if (!WIFEXITED(ws) || WEXITSTATUS(ws) != 0)
		RET(DOFERR);

	/* assert that $1 hasn't changed */
	if (stat(df->arg1, &st) < 0) {
		if (errno != ENOENT)
			perrnand(RET(DOFERR), "stat: %s", df->arg1);
		if (pst.st_size >= 0)
			perrfand(RET(DOFERR), "aborting: .do file has removed $1");
	} else {
		if (pst.st_size < 0)
			perrfand(RET(DOFERR), "aborting: .do file has created $1");
		if (!TSEQ(pst.st_ctim, st.st_ctim))
			perrfand(RET(DOFERR), "aborting: .do file modified $1");
	}

	/* determine whether $3 or stdout is the target */
	trg = NULL;

	if ((a3fd = open(df->arg3, O_RDONLY)) >= 0) /* .do file created $3 */
		trg = df->arg3, unlarg3 = 0;
	else if (errno != ENOENT)
		perrnand(RET(DOFERR), "open: %s", df->arg3);

	if (fstat(fd1, &st) < 0)
		perrnand(RET(DOFERR), "fstat: %s", df->fd1f);
	else if (st.st_size > 0) { /* .do file wrote to stdout */
		if (trg) { /* .do file also created $3 */
			unlarg3 = 1;
			perrf("aborting: .do file created $3 AND wrote to stdout");
			RET(DOFERR);
		}
		trg = df->fd1f, unlfd1f = 0;
	}
	if (!trg)
		RET(TRGSAME);

	/* fsync the target, rename, fsync target's directory */
	if (prog.fsync) {
		if (trg == df->arg3) {
			if (fsync(a3fd) < 0)
				perrnand(RET(DOFERR), "fsync: %s", df->arg3);
		} else if (fsync(fd1) < 0)
			perrnand(RET(DOFERR), "fsync: %s", df->fd1f);
	}
	if (rename(trg, df->arg1) < 0)
		perrnand(RET(DOFERR), "rename: %s -> %s", trg, df->arg1);
	if (prog.fsync)
		DIRFROMPATH(dir, df->arg1,
			if (dirsync(dir) < 0)
				perrnand(RET(DOFERR), "dirsync: %s", dir);
		);
	RET(TRGNEW);
befret:
	if (fd1 >= 0 && close(fd1) < 0)
		perrnand(rv = DOFERR, "close: %s", df->fd1f);
	if (a3fd >= 0 && close(a3fd) < 0)
		perrnand(rv = DOFERR, "close: %s", df->arg3);
	if (unlarg3 && unlink(df->arg3) < 0 && errno != ENOENT)
		perrnand(rv = DOFERR, "unlink: %s", df->arg3);
	if (unlfd1f && unlink(df->fd1f) < 0 && errno != ENOENT)
		perrnand(rv = DOFERR, "unlink: %s", df->fd1f);
	return rv;
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
	if (filelck(depfd, F_SETLKW, F_WRLCK, 0, 0) < 0)
		perrnand(return 0, "filelck");

	if (dprintf(depfd, "%c%s", t, depfnm) < 0 ||
	dowrite(depfd, "", 1) < 0)
		perrfand(return 0, "write");

	if (filelck(depfd, F_SETLK, F_UNLCK, 0, 0) < 0)
		perrnand(return 0, "filelck");
	return 1;
}

int
fputdep(FILE *f, int t, FPARS(const char, *fnm, *trg))
{
	struct stat st;
	char rlp[PATH_MAX], tdir[PATH_MAX];

	fputc(t, f);
	if (t != '-') {
		if (stat(fnm, &st) < 0)
			perrnand(return 0, "stat: %s", fnm);
		fwrite(&st.st_ino, sizeof st.st_ino, 1, f);
		fwrite(&st.st_mtim, sizeof st.st_mtim, 1, f);
	} else {
		if (access(fnm, F_OK) < 0) {
			if (errno != ENOENT)
				perrnand(return 0, "access");
		} else
			perrfand(return 0, "%s: ifcreate dependency error: %s",
				fnm, strerror(EEXIST));
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
	int c, rv;
	char depln[PATH_MAX+1]; /* type, PATH */
	char wrfnm[PATH_MAX];

	rf = wf = NULL;
	sprintf(wrfnm, "%s.t", bifnm); /* write to a temporary file at first */
	if (!(rf = fopen(rdfnm, "r")))
		perrnand(RET(0), "fopen: %s", rdfnm);
	if (!(wf = fopen(wrfnm, "w")))
		perrnand(RET(0), "fopen: %s", wrfnm);

	if (filelck(fileno(wf), F_SETLKW, F_WRLCK, 0, 0) < 0)
		perrnand(RET(0), "filelck: %s", wrfnm);
	if (!fputdep(wf, ':', trg, trg))
		RET(0);
	i = 0;
	while ((c = fgetc(rf)) != EOF) {
		if (!(depln[i++] = c)) {
			if (!fputdep(wf, depln[0], depln+1, trg))
				RET(0);
			i = 0;
			continue;
		}
		if (i >= sizeof depln)
			RET(0);
	}
	if (!feof(rf))
		RET(0);

	/* fsync bifile, rename, fsync directory */
	if (prog.fsync && fsync(fileno(wf)) < 0)
		perrnand(RET(0), "fsync: %s", wrfnm);
	if (rename(wrfnm, bifnm) < 0)
		perrnand(RET(0), "rename: %s -> %s", wrfnm, bifnm);
	if (prog.fsync)
		DIRFROMPATH(dir, wrfnm,
			if (dirsync(dir) < 0)
				perrnand(RET(0), "dirsync: %s", dir);
		);
	RET(1);
befret:
	if (rf && fclose(rf))
		perrnand(rv = 0, "fclose: %s", rdfnm);
	if (wf && fclose(wf))
		perrnand(rv = 0, "fclose: %s", wrfnm);
	return rv;
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
pstatln(FPARS(int, ok, lvl), FPARS(const char, *trg, *dfpth))
{
	int i;
	char rlp[PATH_MAX];

	if (filelck(STDERR_FILENO, F_SETLKW, F_WRLCK, 0, 0) < 0)
		perrnand(return, "filelck: /dev/stderr");

	eprintf("redo %s ", ok ? "ok" : "err");
	for (i = 0; i < lvl; i++)
		eprintf(".");
	eprintf(&" %s"[lvl == 0], relpath(rlp, sizeof rlp, trg, prog.topwd) ? rlp : trg);
	eprintf(" (%s)\n", relpath(rlp, sizeof rlp, dfpth, prog.topwd) ? rlp : dfpth);

	if (filelck(STDERR_FILENO, F_SETLK, F_UNLCK, 0, 0) < 0)
		perrnand(return, "filelck: /dev/stderr");
}

/* acquire an execution lock
   if a lockfile exists then either
   . the file has an active lock (the proccess that has the lock is running)
     which is hold by a redo proccess trying to build the same target, so either
   . . there is a dependency cycle <=> the pid stored in the file equals prog.pid
       (not checked when running jobs in parallel)
   . . or another independent redo is building the target
   . or no lock is active, so the proccess that created the lockfile was killed
     and the pid stored in that is not useful */
int
acqexlck(int *fd, const char *lckfnm)
{
	pid_t pid;
	int lckfd, rv;

	if ((lckfd = open(lckfnm, O_RDWR|O_CREAT|O_CLOEXEC, prog.fmode)) < 0)
		perrnand(RET(LCKERR), "open: '%s", lckfnm);
	if (filelck(lckfd, F_SETLK, F_WRLCK, 0, 2) < 0) {
		if (errno != EAGAIN && errno != EACCES)
			perrnand(RET(LCKERR), "filelck: %s", lckfnm);
		/* lock is held by another proccess, wait until it is safe
		   to read the pid */
		if (filelck(lckfd, F_SETLKW, F_RDLCK, 1, 1) < 0)
			perrnand(RET(LCKERR), "filelck: %s", lckfnm);
		if (doread(lckfd, &pid, sizeof(pid_t)) < 0)
			perrnand(RET(LCKERR), "read: %s", lckfnm);
		if (!prog.withjm && pid == prog.toppid)
			RET(DEPCYCL);
		/* wait until the write lock gets released */
		if (filelck(lckfd, F_SETLKW, F_RDLCK, 0, 2) < 0)
			perrnand(RET(LCKERR), "filelck: %s", lckfnm);
		RET(LCKREL); /* lock has been released */
	}
	if (dowrite(lckfd, &prog.toppid, sizeof(pid_t)) < 0)
		perrnand(RET(LCKERR), "write: %s", lckfnm);
	/* notify that it is safe to read the pid */
	if (filelck(lckfd, F_SETLK, F_UNLCK, 1, 1) < 0)
		perrnand(RET(LCKERR), "filelck: %s", lckfnm);
	*fd = lckfd;
	return LCKACQ;
befret:
	if (lckfd >= 0 && close(lckfd) < 0)
		perrnand(rv = LCKERR, "close");
	return rv;
}

int
redo(char *trg, FPARS(int, lvl, pdepfd))
{
	struct dofile df;
	int depfd, lckfd;
	int ok, ifch, rv;
	char tmp[PATH_MAX], lckfnm[PATH_MAX], tmpdepfnm[sizeof prog.tmpffmt];
	char *s;

	lckfd = -1, ifch = 0;
	if ((depfd = mkstemp(strcpy(tmpdepfnm, prog.tmpffmt))) < 0)
		perrnand(RET(0), "mkstemp: %s", tmpdepfnm);

	if (!finddof(trg, &df, depfd))
		perrfand(RET(0), "no .do file for %s", trg);

	/* chdir to the directory the .do file is in */
	DIRFROMPATH(dir, df.pth,
		if (chdir(dir) < 0)
			perrnand(RET(0), "chdir: %s", dir);
	);

	/* create required path */
	s = (s = strrchr(df.arg1, '/')) ? s+1 : df.arg1;
	sprintf(tmp, "%.*s%s", (int)(s - df.arg1), df.arg1, redir);
	if (mkpath(tmp, prog.dmode) < 0)
		perrnand(RET(0), "mkpath: %s", tmp);

	switch (acqexlck(&lckfd, getlckfnm(lckfnm, trg))) {
	case DEPCYCL:
		perrf("%s: dependency cycle detected", relpath(tmp,
			sizeof tmp, trg, prog.topwd) ? tmp : trg);
	default:
	case LCKERR:
		lckfd = -1;
		RET(0);
	case LCKREL:
		lckfd = -1;
		goto ifchange;
	case LCKACQ:
		break;
	}

	/* exec */
	if ((ok = execdof(&df, lvl+1, depfd)) == DOFINT)
		RET(0);
	pstatln(ok >= TRGSAME, lvl, trg, df.pth);

	if (ok < TRGSAME)
		RET(0);

	if (!access(trg, F_OK)) {
		if (pdepfd >= 0 && !repdep(pdepfd, '=', trg))
			RET(0);
		if (!recdeps(getbifnm(tmp, trg), tmpdepfnm, trg))
			RET(0);
	} else if (errno != ENOENT)
		perrnand(RET(0), "access: %s", trg);
	RET(1);
ifchange:
	ifch = 1, rv = 0;
befret:
	if (depfd >= 0) {
		if (close(depfd) < 0)
			perrnand(rv = 0, "close");
		if (unlink(tmpdepfnm) < 0 && errno != ENOENT)
			perrnand(rv = 0, "unlink: %s", tmpdepfnm);
	}
	if (lckfd >= 0) {
		if (close(lckfd) < 0)
			perrnand(rv = 0, "close");
		if (unlink(lckfnm) < 0 && errno != ENOENT)
			perrnand(rv = 0, "unlink: %s", lckfnm);
	}
	if (ifch)
		rv = redoifchange(trg, lvl, pdepfd);
	return rv;
}

int
redoifchange(char *trg, FPARS(int, lvl, pdepfd))
{
	FILE *bif; /* build info file */
	struct dep dep;
	int tdirfd;
	int rb, c, rv;
	char tdir[PATH_MAX], bifnm[PATH_MAX], depfnm[PATH_MAX];

	rb = 0, tdirfd = -1, bif = NULL;
	/* target doesn't exist */
	if (access(trg, F_OK))
		goto rebuild;

	DIRFROMPATH(dir, trg,
		strcpy(tdir, dir);
	);
	if ((tdirfd = open(tdir, O_RDONLY|O_DIRECTORY|O_CLOEXEC)) < 0)
		perrnand(RET(0), "open: %s", tdir);

	/* when trg exists but build info in .redo/ doesn't,
	   assume trg in not supposed to been build by redo */
	if (access(getbifnm(bifnm, trg), F_OK)) {
		if (pdepfd >= 0 && !repdep(pdepfd, '=', trg))
			RET(0);
		RET(1);
	}

	if (!(bif = fopen(bifnm, "r")))
		perrnand(RET(0), "fopen: %s", bifnm);
	if (filelck(fileno(bif), F_SETLKW, F_RDLCK, 0, 0) < 0)
		perrnand(RET(0), "filelck: %s", bifnm);

	if (!fgetdep(bif, &dep) || dep.type != ':')
		goto rebuild;
	if (depchanged(&dep, tdirfd))
		perrfand(RET(0), "aborting: %s was externally modified",
			dep.fnm);
	while ((c = fgetc(bif)) != EOF) {
		ungetc(c, bif);
		if (!fgetdep(bif, &dep))
			goto rebuild;
		if (dep.type == '=') {
			if (!normpath(depfnm, sizeof depfnm - PTHMAXSUF,
			dep.fnm, tdir)) {
				errno = ENAMETOOLONG;
				perrfand(RET(0), "%s", dep.fnm);
			}
			if (!redoifchange(depfnm, lvl+1, -1))
				RET(0);
		}
		if (depchanged(&dep, tdirfd))
			goto rebuild;
	}
	if (pdepfd >= 0 && !repdep(pdepfd, '=', trg))
		perrnand(RET(0), "repdep: %s", trg);
	RET(1);
rebuild:
	rb = 1, rv = 0;
befret:
	if (tdirfd >= 0 && close(tdirfd) < 0)
		perrnand(rv = 0, "close: %s", tdir);
	if (bif && fclose(bif))
		perrnand(rv = 0, "fclose: %s", bifnm);
	if (rb)
		rv = redo(trg, lvl, pdepfd);
	return rv;
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
	int c, rv;
	char bifnm[PATH_MAX];

	bif = NULL;
	if (!(bif = fopen(getbifnm(bifnm, trg), "r"))) {
		if (errno != ENOENT)
			perrnand(RET(0), "fopen: %s", bifnm);
		char rlp[PATH_MAX];
		printf("%s: not build by redo\n",
			relpath(rlp, sizeof rlp, trg, prog.wd) ? rlp : trg);
		RET(0);
	}

	if (filelck(fileno(bif), F_SETLKW, F_RDLCK, 0, 0) < 0)
		perrnand(RET(0), "filelck: %s", bifnm);
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
		perrnand(RET(0), "ferror: %s", bifnm);
	RET(1);
invlf:
	perrfand(RET(0), "%s: invalid build-info file", bifnm);
befret:
	if (bif && fclose(bif))
		perrnand(rv = 0, "fclose: %s", bifnm);
	return rv;
}

int
fredo(redofnt *redofn, char *targ)
{
	char trg[PATH_MAX];

	if (!normpath(trg, sizeof trg - PTHMAXSUF, targ, prog.wd))
		perrfand(return 0, "%s: %s", targ, strerror(ENAMETOOLONG));

	return (*redofn)(trg, prog.lvl, prog.pdepfd);
}

void
jredo(redofnt *redofn, char *trg, FPARS(int, *paral, hnext))
{
	struct pollfd pfd;
	ssize_t r;
	pid_t cld; /* child's pid, if any */
	int ja, msg, st, rv;

	cld = -1;
	if (hnext || *paral) {
		if (!*paral) { /* check whether a is job available */
			pfd.fd = prog.jmrfd;
			pfd.events = POLLIN;
			if ((ja = poll(&pfd, 1, 0)) < 0)
				perrnand(RET(1), "poll");
		} else {
			msg = JOBNEW;
			if (dowrite(prog.jmwfd, &msg, sizeof msg) < 0)
				perrnand(RET(1), "write");
			if ((r = read(prog.jmrfd, &msg, sizeof msg)) < 0)
				perrnand(RET(1), "read");
			if (!r) /* job manager closed the pipe */
				RET(1);
		}
		if (hnext && (*paral || ja)) {
			if ((cld = fork()) < 0)
				perrnand(RET(1), "fork");
			if (!cld) {
				*paral = 1;
				prog.pid = getpid();
				return;
			}
		}
	}
	if (!(rv = fredo(redofn, trg)) || *paral) {
		msg = rv ? JOBDONE : JOBERR;
		if (dowrite(prog.jmwfd, &msg, sizeof msg) < 0)
			perrnand(RET(1), "write");
	}
	if (!*paral) {
		if (cld >= 0)
			RET(!rv);
		if (rv)
			return;
		exit(1);
	}
	RET(!rv);
befret:
	if (cld >= 0) {
		if (waitpid(cld, &st, 0) < 0)
			perrnand(rv = 1, "waitpid");
		rv = rv || !WIFEXITED(st) || WEXITSTATUS(st);
	}
	exit(rv);
}

void
vredo(redofnt *redofn, int trgc, char *trgv[])
{
	for (; trgc > 0; trgc--, trgv++)
		if (!fredo(redofn, *trgv))
			exit(1);
}

void
vjredo(redofnt *redofn, int trgc, char *trgv[])
{
	int paral;

	paral = 0;
	for (; trgc; trgc--, trgv++)
		jredo(redofn, *trgv, &paral, trgc > 1);
}

/* spawn job manager, non-jm child returns */
void
spawnjm(int jobsn)
{
	pid_t cld;
	int wp[2], rp[2];
	int st, s;

	if (pipe(wp) < 0 || pipe(rp) < 0)
		ferrn("pipe");
	if ((cld = fork()) < 0)
		ferrn("fork");
	else if (!cld) {
		prog.jmwfd = wp[1];
		prog.jmrfd = rp[0];
		if (envseti(enm.jmrfd, prog.jmrfd) < 0 ||
		envseti(enm.jmwfd, prog.jmwfd) < 0)
			ferrn("envseti");
		if (close(wp[0]) < 0 || close(rp[1]) < 0)
			ferrn("close");
		return;
	}
	prognm = "redo-jobmgr";
	if (close(wp[1]) < 0 || close(rp[0]) < 0)
		perrn("close");

	s = !jmrun(jobsn, wp[0], rp[1]);

	if (waitpid(cld, &st, 0) < 0)
		perrnand(s = 1, "wait");
	exit(s || !WIFEXITED(st) || WEXITSTATUS(st));
}

void
onsig(int sig)
{
	if (prog.retonsig)
		return;
	_exit(1);
}

void
setup(int jobsn)
{
	struct sigaction sa;
	mode_t mask;
	const char *d;
	char *e;
	size_t n;

	sa = (struct sigaction){.sa_handler = &onsig};
	if (sigaction(SIGINT, &sa, NULL) < 0)
		ferrn("sigaction");

	prog.withjm = 0;
	if ((prog.jmrfd = envgetfd(enm.jmrfd)) < 0) {
		if (jobsn) {
			spawnjm(jobsn);
			prog.withjm = 1;
		}
	} else {
		prog.withjm = 1;
		if ((prog.jmwfd = envgetfd(enm.jmwfd)) < 0)
			ferrf("invalid environment values for %s and %s",
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
			ferrf("invalid environment variable %s", enm.toppid);
		if ((prog.pdepfd = envgetfd(enm.pdepfd)) < 0)
			ferrf("invalid environment variable %s", enm.pdepfd);
		if (!(e = getenv(enm.topwd)))
			ferrf("invalid environment values for %s and %s",
				enm.lvl, enm.topwd);
		strlcpy(prog.topwd, e, sizeof prog.topwd);
	}
	d = (d = getenv("TMPDIR")) ? d : "/tmp";
	n = sizeof prog.tmpffmt;
	if (snprintf(prog.tmpffmt, n, "%s/redo.tmp.XXXXXX", d) >= n)
		ferrf("$TMPDIR: %s", strerror(ENAMETOOLONG));

	prog.fsync = envgeti(enm.fsync, 0, 1, 1);
}

void
usage(void)
{
	ferrf("usage: redo [-j n] targets...");
}

int
main(int argc, char *argv[])
{
	redofnt *redofn;
	int jobsn;
	char *s;

	prognm = (s = strrchr(argv[0], '/')) ? s+1 : argv[0];

	jobsn = 1;
	ARGBEGIN {
	case 'j':
		if (!(s = ARGF()))
			perrfand(usage(), "missing argument for -j");
		if ((jobsn = strtoint(s, 0, INT_MAX, -1)) < 0)
			perrfand(usage(), "%s: Invalid number", s);
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
		ferrf("%s: not implemented", prognm);

	setup(jobsn - 1);
	if (prog.withjm)
		vjredo(redofn, argc, argv);
	else
		vredo(redofn, argc, argv);

	return 0;
}
