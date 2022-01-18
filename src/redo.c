#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include "util.h"

/* max number of chars added to valid paths as suffix */
#define PTHMAXSUF  (sizeof redir + NAME_MAX)
#define TSEQ(A, B) ((A).tv_sec == (B).tv_sec && (A).tv_nsec == (B).tv_nsec)
#define DIRFROMABS(D, RLP, CODE) \
	do { \
		char *s_, *D; \
		if ((s_ = strrchr(RLP, '/')) > (RLP)) \
			D = (RLP), *s_ = '\0'; \
		else \
			D = "/"; \
		CODE \
		if (s_ > (RLP)) \
			*s_ = '/'; \
	} while (0)
#define DIRFROMREL(D, RLP, CODE) \
	do { \
		char *s_, *D; \
		if ((s_ = strrchr((RLP), '/'))) \
			D = (RLP), *s_ = '\0'; \
		else \
			D = "."; \
		CODE \
		if (s_) \
			*s_ = '/'; \
	} while (0)

/* possible outcomes of executing a .do file */
enum { DOFERR, TRGPHONY, TRGOK, };

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
	struct timespec ctim;
	char fnm[PATH_MAX];
};

struct {
	pid_t pid, toppid;
	mode_t dmode, fmode;
	int lvl;
	int pdepfd; /* fd in which parents expects dependency reporting */
	char topwd[PATH_MAX];
	char wd[PATH_MAX];
	char tmpffmt[PATH_MAX];
} prog;

struct {
	const char *lvl;
	const char *topwd;
	const char *toppid;
	const char *pdepfd;
} enm = { /* environment variables names */
	.lvl    = "REDO_LEVEL",
	.topwd  = "REDO_TOPWD",
	.toppid = "REDO_TOPPID",
	.pdepfd = "REDO_DEPFD",
};

const char *prognm;
const char shell[]      = "/bin/sh";
const char shellflags[] = "-e";
const char redir[]      = ".redo"; /* directory redo expects to use exclusively */

/* function declerations */
int envgetn(const char *name, FPARS(long long, min, max, def));
int envsetn(const char *name, long long n);
char *normpath(char *abs, size_t n, FPARS(const char, *path, *relto));
char *relpath(char *rlp, size_t n, FPARS(const char, *path, *relto));
int mkpath(char *path, mode_t mode);
int filelck(FPARS(int, fd, cmd, type), FPARS(off_t, start, len));
int dirsync(const char *dpth);
int dofisok(const char *pth, int depfd);
int finddof(const char *trg, struct dofile *df, int depfd);
int execdof(struct dofile *fd, FPARS(int, lvl, depfd));
char *redirentry(char *fnm, FPARS(const char, *trg, *suf));
char *getlckfnm(char *fnm, const char *trg);
char *getbifnm(char *fnm, const char *trg);
int repdep(int depfd, char t, const char *trg);
int fputdep(FILE *f, int t, FPARS(const char, *fnm, *trg));
int recdeps(FPARS(const char, *bifnm, *rdfnm, *trg));
int fgetdep(FILE *f, struct dep *dep);
int depchanged(struct dep *dep, int tdirfd);
void prstatln(FPARS(int, ok, lvl), FPARS(const char, *trg, *dfpth));
int acqexlck(int *fd, const char *lckfnm);
int redo(char *trg, FPARS(int, lvl, pdepfd));
int redoifchange(char *trg, FPARS(int, lvl, pdepfd));
int redoifcreate(char *trg, FPARS(int, lvl, pdepfd));
int redoinfofor(char *trg, FPARS(int, lvl, pdepfd));
void vredo(RedoFn redofn, char *trgv[]);
void setup(void);

/* function implementations */
int
envgetn(const char *name, FPARS(long long, min, max, def))
{
	long long n;
	char *v, *e;

	if (!(v = getenv(name)))
		return def;
	n = strtoll(v, &e, 10);
	if (*v && *e)
		return def;
	if (n < min || n > max)
		return def;
	return n;
}

int
envsetn(const char *name, long long n)
{
	char s[32];

	snprintf(s, sizeof s, "%lld", n);
	return setenv(name, s, 1);
}

/* convert path to a normalized absolute path in abs,
   with no ., .. components, and double slashes. if path is not absolute,
   it assumed that is relative to `relto`, which must be normalized path,
   like /this/nice/abs/path
   return NULL if the nul-terminated result wouldn't fit in n chars */
char *
normpath(char *abs, size_t n, FPARS(const char, *path, *relto))
{
	const char *s;
	char *d;

	d = abs, s = path;
	if (path[0] != '/') {
		if (n <= strlen(relto))
			return NULL;
		d = stpcpy(abs, relto);
		n -= strlen(relto)+1;
	}
	*d++ = '/';
	while (*s) {
		/* d[-1] == '/' */
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
		while (*s && *s != '/') {
			if (!n--)
				return NULL;
			*d++ = *s++;
		}
		*d++ = '/';
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
		perrand(ret(DOFERR), "mkstemp: '%s'", df->fd1f);
	unlfd1f = 1;
	if (fchmod(fd1, prog.fmode) < 0) /* respect umask */
		perrand(ret(DOFERR), "fchmod: '%s'", df->fd1f);

	sprintf(df->arg3, "%s.%d", df->fd1f, prog.pid);
	if (!access(df->arg3, F_OK))
		perrfand(ret(DOFERR), "assertion failed: '%s' exists",
			df->arg3);

	/* get $1's status to later assert that it has not changed */
	if (lstat(df->arg1, &pst) < 0) {
		if (errno != ENOENT)
			perrand(ret(DOFERR), "lstat: '%s'", df->arg1);
		pst.st_size = -1;
	} else
		pst.st_size = 0;

	if ((pid = fork()) < 0)
		perrand(ret(DOFERR), "fork");
	if (pid == 0) {
		envsetn(enm.pdepfd, depfd);
		envsetn(enm.lvl, lvl);

		if (dup2(fd1, STDOUT_FILENO) < 0)
			ferr("dup2");
		if (!access(df->pth, X_OK)) {
			execl(df->pth, df->pth, df->arg1, df->arg2, df->arg3,
				(char *)0);
			ferr("execl");
		}
		execl(shell, shell, shellflags, df->pth, df->arg1, df->arg2,
			df->arg3, (char *)0);
		ferr("execl");
	}
	wait(&ws);
	if (!WIFEXITED(ws) || WEXITSTATUS(ws) != 0)
		ret(DOFERR);
	unlarg3 = 1;

	/* assert that $1 hasn't changed */
	if (lstat(df->arg1, &st) < 0) {
		if (errno != ENOENT)
			perrand(ret(DOFERR), "lstat: '%s'", df->arg1);
		if (pst.st_size >= 0)
			perrfand(ret(DOFERR), "aborting: .do file has removed $1");
	} else if (!TSEQ(pst.st_ctim, st.st_ctim))
		perrfand(ret(DOFERR), "aborting: .do file modified $1");

	/* determine whether $3 or stdout is the target */
	trg = NULL;
	if (!access(df->arg3, F_OK)) { /* .do file created $3 */
		trg = df->arg3, unlarg3 = 0;
		if ((a3fd = open(df->arg3, O_RDONLY)) < 0)
			perrand(ret(DOFERR), "open: '%s'", df->arg3);
	} else if (errno != ENOENT)
		perrand(ret(DOFERR), "stat: '%s'", df->arg3);
	if (stat(df->fd1f, &st) < 0)
		perrand(ret(DOFERR), "stat: '%s'", df->fd1f);
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
	if (trg == df->arg3) {
		if (fsync(a3fd) < 0)
			perrand(ret(DOFERR), "fsync: '%s'", df->arg3);
	} else if (fsync(fd1) < 0)
		perrand(ret(DOFERR), "fsync: '%s'", df->fd1f);
	if (rename(trg, df->arg1) < 0)
		perrand(ret(DOFERR), "rename: '%s' -> '%s'", trg, df->arg1);
	DIRFROMREL(dir, df->arg1,
		if (dirsync(dir) < 0)
			perrand(ret(DOFERR), "dirsync: '%s'", dir);
	);

	ret(TRGOK);
end:
	if (fd1 >= 0 && close(fd1) < 0)
		perrand(r = DOFERR, "close: '%s'", df->fd1f);
	if (a3fd >= 0 && close(a3fd) < 0)
		perrand(r = DOFERR, "close: '%s'", df->arg3);
	if (unlarg3 && unlink(df->arg3) < 0 && errno != ENOENT)
		perrand(r = DOFERR, "unlink: '%s'", df->arg3);
	if (unlfd1f && unlink(df->fd1f) < 0 && errno != ENOENT)
		perrand(r = DOFERR, "unlink: '%s'", df->fd1f);
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
		if (lstat(fnm, &st))
			perrand(return 0, "lstat: '%s'", fnm);
		fwrite(&st.st_ctim, sizeof st.st_ctim, 1, f);
	}
	strlcpy(tdir, trg, strrchr(trg, '/') - trg);
	if (!relpath(rlp, sizeof rlp, fnm, tdir))
		perrand(return 0, "%s", strerror(ENAMETOOLONG));
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
		perrand(ret(0), "fopen: '%s'", rdfnm);
	rfopen = 1;
	if (!(wf = fopen(wrfnm, "w")))
		perrand(ret(0), "fopen: '%s'", wrfnm);
	wfopen = 1;

	if (filelck(fileno(wf), F_SETLKW, F_WRLCK, 0, 0) < 0)
		perrand(ret(0), "filelck: '%s'", wrfnm);
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
	if (fsync(fileno(wf)) < 0)
		perrand(ret(0), "fsync: '%s'", wrfnm);
	if (rename(wrfnm, bifnm) < 0)
		perrand(ret(0), "rename: '%s' -> '%s'", wrfnm, bifnm);
	DIRFROMABS(dir, wrfnm,
		if (dirsync(dir) < 0)
			perrand(ret(0), "dirsync: '%s'", dir);
	);
	ret(1);
end:
	if (rfopen && fclose(rf) == EOF)
		perrand(r = 0, "fclose: '%s'", rdfnm);
	if (wfopen && fclose(wf) == EOF)
		perrand(r = 0, "fclose: '%s'", wrfnm);
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
		if (fread(&dep->ctim, sizeof dep->ctim, 1, f) != 1)
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
		if (!fstatat(tdirfd, dep->fnm, &st, AT_SYMLINK_NOFOLLOW) &&
		TSEQ(dep->ctim, st.st_ctim))
			return 0;
	bcase '-':
		if (faccessat(tdirfd, dep->fnm, F_OK, AT_SYMLINK_NOFOLLOW))
			return 0;
	}
	return 1;
}

void
prstatln(FPARS(int, ok, lvl), FPARS(const char, *trg, *dfpth))
{
	const char *p;
	char rlp[PATH_MAX];
	int i;

	eprintf("redo %s ", ok ? "ok" : "err");
	for (i = 0; i < lvl; i++)
		eprintf(". ");

	p = relpath(rlp, sizeof rlp, trg, prog.topwd) ? rlp : trg;
	eprintf("%s", p);

	p = relpath(rlp, sizeof rlp, dfpth, prog.topwd) ? rlp : dfpth;
	eprintf(" (%s)\n", p);
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
		perrand(ret(LCKERR), "open: '%s", lckfnm);
	if (filelck(lckfd, F_SETLK, F_WRLCK, 0, 2) < 0) {
		if (errno != EAGAIN && errno != EACCES)
			perrand(ret(LCKERR), "filelck: '%s'", lckfnm);
		/* lock is held by another proccess, wait until it is safe
		   to read the pid */
		if (filelck(lckfd, F_SETLKW, F_RDLCK, 1, 1) < 0)
			perrand(ret(LCKERR), "filelck: '%s'", lckfnm);
		if (doread(lckfd, &pid, sizeof(pid_t)) < 0)
			perrand(ret(LCKERR), "read: '%s'", lckfnm);
		if (pid == prog.toppid)
			ret(DEPCYCL);
		/* wait until the write lock gets released */
		if (filelck(lckfd, F_SETLKW, F_WRLCK, 0, 2) < 0)
			perrand(ret(LCKERR), "filelck: '%s'", lckfnm);
		ret(LCKREL); /* lock has been released */
	}
	if (dowrite(lckfd, &prog.toppid, sizeof(pid_t)) < 0)
		perrand(ret(LCKERR), "write: '%s'", lckfnm);
	/* notify that it is safe to read the pid */
	if (filelck(lckfd, F_SETLK, F_UNLCK, 1, 1) < 0)
		perrand(ret(LCKERR), "filelck: '%s'", lckfnm);
	*fd = lckfd;
	return LCKACQ;
end:
	if (lckfd >= 0 && close(lckfd) < 0)
		perrand(r = LCKERR, "close");
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
		perrand(ret(0), "mkstemp: '%s'", tmpdepfnm);

	if (!finddof(trg, &df, depfd))
		perrfand(ret(0), "no .do file for '%s'", trg);

	/* chdir to the directory the .do file is in */
	DIRFROMABS(dir, df.pth,
		if (chdir(dir) < 0)
			perrand(ret(0), "chdir: '%s'", dir);
	);

	/* create required path */
	s = (s = strrchr(df.arg1, '/')) ? s+1 : df.arg1;
	sprintf(tmp, "%.*s%s", (int)(s - df.arg1), df.arg1, redir);
	if (mkpath(tmp, prog.dmode) < 0)
		perrand(ret(0), "mkpath: '%s'", tmp);

	switch (acqexlck(&lckfd, getlckfnm(lckfnm, trg))) {
	case DEPCYCL:
		perrf("'%s': dependency cycle detected", relpath(tmp,
			sizeof tmp, trg, prog.topwd) ? tmp : trg);
		/* fallthrough */
	default:
		/* fallthrough */
	case LCKERR:
		lckfd = -1;
		ret(0);
	bcase LCKREL:
		lckfd = -1;
		ret(redoifchange(trg, lvl, pdepfd));
	bcase LCKACQ:
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
			perrand(r = 0, "close");
		if (unlink(tmpdepfnm) < 0 && errno != ENOENT)
			perrand(r = 0, "unlink: '%s'", tmpdepfnm);
	}
	if (lckfd >= 0) {
		if (close(lckfd) < 0)
			perrand(r = 0, "close");
		if (unlink(lckfnm) < 0 && errno != ENOENT)
			perrand(r = 0, "unlink: '%s'", lckfnm);
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

	DIRFROMABS(dir, trg,
		strcpy(tdir, dir);
	);
	if ((tdirfd = open(tdir, O_RDONLY|O_DIRECTORY|O_CLOEXEC)) < 0)
		perrand(ret(0), "open: '%s'", tdir);

	/* when trg exists but build info in .redo/ doesn't,
	   assume trg in not supposed to been build by redo */
	if (access(getbifnm(bifnm, trg), F_OK)) {
		if (pdepfd >= 0 && !repdep(pdepfd, '=', trg))
			ret(0);
		ret(1);
	}

	if (!(bif = fopen(bifnm, "r")))
		perrand(ret(0), "fopen: '%s'", bifnm);
	clbif = 1;
	if (filelck(fileno(bif), F_SETLKW, F_RDLCK, 0, 0) < 0)
		perrand(ret(0), "filelck: '%s'", bifnm);

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
		perrand(ret(0), "repdep: '%s'", trg);
	ret(1);
rebuild:
	ret(redo(trg, lvl, pdepfd));
end:
	if (tdirfd >= 0 && close(tdirfd) < 0)
		perrand(r = 0, "close: '%s'", tdir);
	if (clbif && fclose(bif) == EOF)
		perrand(r = 0, "fclose: '%s'", bifnm);
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
			perrand(ret(0), "fopen: '%s'", bifnm);
		char rlp[PATH_MAX];
		printf("'%s': not build by redo\n",
			relpath(rlp, sizeof rlp, trg, prog.wd) ? rlp : trg);
		ret(0);
	}
	clbif = 1;

	if (filelck(fileno(bif), F_SETLKW, F_RDLCK, 0, 0) < 0)
		perrand(ret(0), "filelck: '%s'", bifnm);
	do {
		if (!fgetdep(bif, &dep))
			goto invlf;
		printf("%c ", (char)dep.type);
		if (dep.type != '-')
			printf("%lld %lld ", (long long)dep.ctim.tv_sec,
				(long long)dep.ctim.tv_nsec);
		printf("%s\n", dep.fnm);
		if ((c = fgetc(bif)) == EOF)
			break;
		ungetc(c, bif);
	} while (1);
	if (ferror(bif))
		perrand(ret(0), "ferror: '%s'", bifnm);
	ret(1);
invlf:
	perrfand(ret(0), "'%s': invalid build-info file", bifnm);
end:
	if (clbif && fclose(bif) == EOF)
		perrand(r = 0, "fclose: '%s'", bifnm);
	return r;
#undef ret
}

void
vredo(RedoFn redofn, char *trgv[])
{
	char trg[PATH_MAX];

	for (; *trgv; trgv++) {
		if (!normpath(trg, sizeof trg - PTHMAXSUF, *trgv, prog.wd))
			ferrf("'%s': %s", *trgv, strerror(ENAMETOOLONG));
		if (!redofn(trg, prog.lvl, prog.pdepfd))
			exit(1);
	}
}

void
setup(void)
{
	mode_t mask;
	const char *d;
	char *e;
	size_t n;

	umask(mask = umask(0)); /* get and restore umask */
	prog.fmode = (prog.dmode = 0777 & ~mask) & ~0111;
	prog.pid = getpid();

	if (!getcwd(prog.wd, sizeof prog.wd))
		ferr("getcwd");

	if (!(prog.lvl = envgetn(enm.lvl, 1, INT_MAX, 0))) {
		strcpy(prog.topwd, prog.wd);
		prog.toppid = prog.pid;
		prog.pdepfd = -1;

		setenv(enm.topwd, prog.wd, 1);
		envsetn(enm.toppid, prog.toppid);
	} else {
		if ((prog.toppid = envgetn(enm.toppid, 1, INT_MAX, -1)) < 0)
			ferrf("invalid environment variable '%s'", enm.toppid);
		if ((prog.pdepfd = envgetn(enm.pdepfd, 0, INT_MAX, -1)) < 0)
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
}

int
main(int argc, char *argv[])
{
	RedoFn redofn;
	char *tmp;

	prognm = (tmp = strrchr(argv[0], '/')) ? tmp+1 : argv[0];
	argc--, argv++;

	setup();
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
	vredo(redofn, argv);

	return 0;
}
