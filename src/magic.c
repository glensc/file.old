/*
 * Copyright (c) Christos Zoulas 2003.
 * All Rights Reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice immediately at the beginning of the file, without modification,
 *    this list of conditions, and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *  
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "magic.h"
#include "file.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/param.h>	/* for MAXPATHLEN */
#include <sys/stat.h>
#include <fcntl.h>	/* for open() */
#ifdef QUICK
#include <sys/mman.h>
#endif

#if defined(HAVE_UTIME)
# if defined(HAVE_SYS_UTIME_H)
#  include <sys/utime.h>
# elif defined(HAVE_UTIME_H)
#  include <utime.h>
# endif
#elif defined(HAVE_UTIMES)
# include <sys/time.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>	/* for read() */
#endif

#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif

#include <netinet/in.h>		/* for byte swapping */

#include "patchlevel.h"

#ifndef	lint
FILE_RCSID("@(#)$Id: magic.c,v 1.10 2003/07/10 21:07:14 christos Exp $")
#endif	/* lint */

#ifdef __EMX__
private char *apptypeName = NULL;
protected int file_os2_apptype(struct magic_set *ms, const char *fn,
    const void *buf, size_t nb);
#endif /* __EMX__ */

#ifndef MAGIC
# define MAGIC "/etc/magic"
#endif

private void free_mlist(struct mlist *);
private void close_and_restore(const struct magic_set *, const char *, int,
    const struct stat *);

public struct magic_set *
magic_open(int flags)
{
	struct magic_set *ms;

	if ((ms = malloc(sizeof(struct magic_set))) == NULL)
		return NULL;

	if (magic_setflags(ms, flags) == -1) {
		free(ms);
		errno = EINVAL;
		return NULL;
	}

	ms->o.ptr = ms->o.buf = malloc(ms->o.size = 1024);
	ms->o.len = 0;
	if (ms->o.buf == NULL) {
		free(ms);
		return NULL;
	}
	ms->c.off = malloc((ms->c.len = 10) * sizeof(*ms->c.off));
	if (ms->c.off == NULL) {
		free(ms->o.buf);
		free(ms);
		return NULL;
	}
	ms->haderr = 0;
	ms->mlist = NULL;
	return ms;
}

/*
 * load a magic file
 */
public int
magic_load(struct magic_set *ms, const char *magicfile)
{
	struct mlist *ml;

	if (magicfile == NULL)
		magicfile = (ms->flags & MAGIC_MIME) ? MAGIC ".mime" : MAGIC;

	ml = file_apprentice(ms, magicfile, 0);
	if (ml == NULL) 
		return -1;
	free_mlist(ms->mlist);
	ms->mlist = ml;
	return 0;
}

private void
free_mlist(struct mlist *mlist)
{
	struct mlist *ml;

	if (mlist == NULL)
		return;

	for (ml = mlist->next; ml != mlist;) {
		struct mlist *next = ml->next;
		struct magic *mg = ml->magic;
		switch (ml->mapped) {
		case 0:
			free(mg);
			break;
		case 1:
			mg--;
			free(mg);
			break;
		case 2:
			mg--;
			(void)munmap(mg, sizeof(*mg) * (ml->nmagic + 1));
			break;
		}
		free(ml);
		ml = next;
	}
	free(ml);
}

public void
magic_close(ms)
    struct magic_set *ms;
{
	free_mlist(ms->mlist);
	free(ms->o.buf);
	free(ms->c.off);
	free(ms);
}

public int
magic_compile(struct magic_set *ms, const char *magicfile)
{
	struct mlist *ml = file_apprentice(ms, magicfile, FILE_COMPILE);
	if(ml == NULL)
		return -1;
	free_mlist(ml);
	return ml ? 0 : -1;
}

public int
magic_check(struct magic_set *ms, const char *magicfile)
{
	struct mlist *ml = file_apprentice(ms, magicfile, FILE_CHECK);
	if(ml == NULL)
		return -1;
	free_mlist(ml);
	return ml ? 0 : -1;
}

private void
close_and_restore(const struct magic_set *ms, const char *name, int fd,
    const struct stat *sb)
{
	(void) close(fd);
	if (fd != STDIN_FILENO && (ms->flags & MAGIC_PRESERVE_ATIME) != 0) {
		/*
		 * Try to restore access, modification times if read it.
		 * This is really *bad* because it will modify the status
		 * time of the file... And of course this will affect
		 * backup programs
		 */
#ifdef HAVE_UTIMES
		struct timeval  utsbuf[2];
		utsbuf[0].tv_sec = sb->st_atime;
		utsbuf[1].tv_sec = sb->st_mtime;

		(void) utimes(name, utsbuf); /* don't care if loses */
#elif defined(HAVE_UTIME_H) || defined(HAVE_SYS_UTIME_H)
		struct utimbuf  utbuf;

		utbuf.actime = sb->st_atime;
		utbuf.modtime = sb->st_mtime;
		(void) utime(name, &utbuf); /* don't care if loses */
#endif
	}
}

/*
 * find type of named file
 */
public const char *
magic_file(struct magic_set *ms, const char *inname)
{
	int	fd = 0;
	unsigned char buf[HOWMANY+1];	/* one extra for terminating '\0' */
	struct stat	sb;
	ssize_t nbytes = 0;	/* number of bytes read from a datafile */

	if (file_reset(ms) == -1)
		return NULL;

	switch (file_fsmagic(ms, inname, &sb)) {
	case -1:
		return NULL;
	case 0:
		break;
	default:
		return ms->o.buf;
	}

#ifndef	STDIN_FILENO
#define	STDIN_FILENO	0
#endif
	if (inname == NULL)
		fd = STDIN_FILENO;
	else if ((fd = open(inname, O_RDONLY)) < 0) {
		/* We can't open it, but we were able to stat it. */
		if (sb.st_mode & 0002)
			if (file_printf(ms, "writable, ") == -1)
				return NULL;
		if (sb.st_mode & 0111)
			if (file_printf(ms, "executable, ") == -1)
				return NULL;
		return ms->o.buf;
	}

	/*
	 * try looking at the first HOWMANY bytes
	 */
	if ((nbytes = read(fd, (char *)buf, HOWMANY)) == -1) {
		file_error(ms, "Cannot read `%s' %s", inname, strerror(errno));
		goto done;
	}

	if (nbytes == 0) {
		if (file_printf(ms, (ms->flags & MAGIC_MIME) ?
		    "application/x-empty" : "empty") == -1) {
			(void)close(fd);
		    goto done;
		}
	} else {
		buf[nbytes++] = '\0';	/* null-terminate it */
#ifdef __EMX__
		switch (file_os2_apptype(ms, inname, buf, nbytes)) {
		case -1:
			goto done;
		case 0:
			break;
		default:
			return ms->o.buf;
		}
#endif
		if (file_buffer(ms, buf, (size_t)nbytes) == -1)
			goto done;
#ifdef BUILTIN_ELF
		if (nbytes > 5) {
			/*
			 * We matched something in the file, so this *might*
			 * be an ELF file, and the file is at least 5 bytes
			 * long, so if it's an ELF file it has at least one
			 * byte past the ELF magic number - try extracting
			 * information from the ELF headers that can't easily
			 * be extracted with rules in the magic file.
			 */
			file_tryelf(ms, fd, buf, (size_t)nbytes);
		}
#endif
	}

	close_and_restore(ms, inname, fd, &sb);
	return ms->haderr ? NULL : ms->o.buf;
done:
	close_and_restore(ms, inname, fd, &sb);
	return NULL;
}


public const char *
magic_buffer(struct magic_set *ms, const void *buf, size_t nb)
{
	if (file_reset(ms) == -1)
		return NULL;
	/*
	 * The main work is done here!
	 * We have the file name and/or the data buffer to be identified. 
	 */
	if (file_buffer(ms, buf, nb) == -1) {
		return NULL;
	}
	return ms->haderr ? NULL : ms->o.buf;
}

public const char *
magic_error(struct magic_set *ms)
{
	return ms->haderr ? ms->o.buf : NULL;
}

public int
magic_setflags(struct magic_set *ms, int flags)
{
#if !defined(HAVE_UTIME) && !defined(HAVE_UTIMES)
	if (flags & MAGIC_PRESERVE_ATIME)
		return -1;
#endif
	ms->flags = flags;
	return 0;
}
