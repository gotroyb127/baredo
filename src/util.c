#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#include "util.h"

ssize_t
dowrite(int fd, const void *buf, size_t n)
{
	const char *p;
	ssize_t w;
	size_t l;

	for (l = n, p = buf; l > 0; l -= w, p += w)
		if ((w = write(fd, p, l)) < 0)
			return -1;
	return n;
}

ssize_t
doread(int fd, void *buf, size_t n)
{
	char *p;
	ssize_t r;
	size_t l;

	for (l = n, p = buf; l > 0; l -= r, p += r)
		if ((r = read(fd, p, l)) <= 0)
			return -1;
	return n;
}

size_t
strlcpy(char *d, const char *s, size_t n)
{
	size_t len;

	if (n == 0)
		return strlen(s);
	for (len = 0; n-- > 0 && *s; len++)
		*d++ = *s++;
	*d = '\0';

	if (*s)
		return len + strlen(s);
	return len;
}

const char *
pthpcmp(FPARS(const char, *a, *b))
{
	const char *l = a;

	for (; *a && (*a == *b || !*b); a++, b++) {
		if (*a == '/')
			l = a + 1;
		if (!*b)
			break;
	}

	return l;
}
