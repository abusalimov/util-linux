/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * This file is part of libmount from util-linux project.
 *
 * Copyright (C) 2009-2018 Karel Zak <kzak@redhat.com>
 *
 * libmount is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 */
#ifdef HAVE_SCANDIRAT
#ifndef __USE_GNU
#define __USE_GNU
#endif	/* !__USE_GNU */
#endif	/* HAVE_SCANDIRAT */

#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "fileutils.h"
#include "mangle.h"
#include "mountP.h"
#include "pathnames.h"
#include "strutils.h"

struct libmnt_parser {
	FILE	*f;		/* fstab, mtab, swaps or mountinfo ... */
	const char *filename;	/* file name or NULL */
	char	*buf;		/* buffer (the current line content) */
	size_t	bufsiz;		/* size of the buffer */
	size_t	line;		/* current line */

	pid_t	tid;		/* task ID */

	unsigned int	has_root_fs : 1;	/* root fs aread parsed */
};

static void parser_cleanup(struct libmnt_parser *pa)
{
	if (!pa)
		return;
	free(pa->buf);
	memset(pa, 0, sizeof(*pa));
}

static const char *next_s32(const char *s, int *num, int *rc)
{
	char *end = NULL;

	if (!s || !*s)
		return s;

	*rc = -EINVAL;
	*num = strtol(s, &end, 10);
	if (end == NULL || s == end)
	       return s;
	if (*end == ' ' || *end == '\t' || *end == '\0')
		*rc = 0;
	return end;
}

static const char *next_u64(const char *s, uint64_t *num, int *rc)
{
	char *end = NULL;

	if (!s || !*s)
		return s;

	*rc = -EINVAL;
	*num = (uint64_t) strtoumax(s, &end, 10);
	if (end == NULL || s == end)
	       return s;
	if (*end == ' ' || *end == '\t' || *end == '\0')
		*rc = 0;
	return end;
}

static inline const char *skip_separator(const char *p)
{
	while (p && (*p == ' ' || *p == '\t'))
		++p;
	return p;
}

static inline const char *skip_nonspearator(const char *p)
{
	while (p && *p && !(*p == ' ' || *p == '\t'))
		p++;
	return p;
}

/*
 * Parses one line from {fs,m}tab
 */
static int mnt_parse_table_line(struct libmnt_fs *fs, const char *s)
{
	int rc = 0;
	char *p = NULL;

	fs->passno = fs->freq = 0;

	/* (1) source */
	p = unmangle(s, &s);
	if (!p || (rc = __mnt_fs_set_source_ptr(fs, p))) {
		DBG(TAB, ul_debug("tab parse error: [source]"));
		free(p);
		goto fail;
	}

	s = skip_separator(s);

	/* (2) target */
	fs->target = unmangle(s, &s);
	if (!fs->target) {
		DBG(TAB, ul_debug("tab parse error: [target]"));
		goto fail;
	}

	s = skip_separator(s);

	/* (3) FS type */
	p = unmangle(s, &s);
	if (!p || (rc = __mnt_fs_set_fstype_ptr(fs, p))) {
		DBG(TAB, ul_debug("tab parse error: [fstype]"));
		free(p);
		goto fail;
	}

	s = skip_separator(s);

	/* (4) options (optional) */
	p = unmangle(s, &s);
	if (p && (rc = mnt_fs_set_options(fs, p))) {
		DBG(TAB, ul_debug("tab parse error: [options]"));
		free(p);
		goto fail;
	}
	if (!p)
		goto done;
	free(p);

	s = skip_separator(s);
	if (!s || !*s)
		goto done;

	/* (5) freq (optional) */
	s = next_s32(s, &fs->freq, &rc);
	if (s && *s && rc) {
		DBG(TAB, ul_debug("tab parse error: [freq]"));
		goto fail;
	}

	s = skip_separator(s);
	if (!s || !*s)
		goto done;

	/* (6) passno (optional) */
	s = next_s32(s, &fs->passno, &rc);
	if (s && *s && rc) {
		DBG(TAB, ul_debug("tab parse error: [passno]"));
		goto fail;
	}

done:
	return 0;
fail:
	if (rc == 0)
		rc = -EINVAL;
	DBG(TAB, ul_debug("tab parse error on: '%s' [rc=%d]", s, rc));
	return rc;
}


/*
 * Parses one line from a mountinfo file
 */
static int mnt_parse_mountinfo_line(struct libmnt_fs *fs, const char *s)
{
	int rc = 0;
	unsigned int maj, min;
	char *p;

	fs->flags |= MNT_FS_KERNEL;

	/* (1) id */
	s = next_s32(s, &fs->id, &rc);
	if (!s || !*s || rc) {
		DBG(TAB, ul_debug("tab parse error: [id]"));
		goto fail;
	}

	s = skip_separator(s);

	/* (2) parent */
	s = next_s32(s, &fs->parent, &rc);
	if (!s || !*s || rc) {
		DBG(TAB, ul_debug("tab parse error: [parent]"));
		goto fail;
	}

	s = skip_separator(s);

	/* (3) maj:min */
	if (sscanf(s, "%u:%u", &maj, &min) != 2) {
		DBG(TAB, ul_debug("tab parse error: [maj:min]"));
		goto fail;
	}
	fs->devno = makedev(maj, min);
	s = skip_nonspearator(s);
	s = skip_separator(s);

	/* (4) mountroot */
	fs->root = unmangle(s, &s);
	if (!fs->root) {
		DBG(TAB, ul_debug("tab parse error: [mountroot]"));
		goto fail;
	}

	s = skip_separator(s);

	/* (5) target */
	fs->target = unmangle(s, &s);
	if (!fs->target) {
		DBG(TAB, ul_debug("tab parse error: [target]"));
		goto fail;
	}

	/* remove "\040(deleted)" suffix */
	p = (char *) endswith(fs->target, PATH_DELETED_SUFFIX);
	if (p && *p)
		*p = '\0';

	s = skip_separator(s);

	/* (6) vfs options (fs-independent) */
	fs->vfs_optstr = unmangle(s, &s);
	if (!fs->vfs_optstr) {
		DBG(TAB, ul_debug("tab parse error: [VFS options]"));
		goto fail;
	}

	/* (7) optional fields, terminated by " - " */
	p = strstr(s, " - ");
	if (!p) {
		DBG(TAB, ul_debug("mountinfo parse error: separator not found"));
		return -EINVAL;
	}
	if (p > s + 1) {
		fs->opt_fields = strndup(s + 1, p - s - 1);
		if (!fs->opt_fields)
			goto fail;

		__mnt_fs_set_propagation_from_string(fs, fs->opt_fields);
	}

	s = skip_separator(p + 3);

	/* (8) FS type */
	p = unmangle(s, &s);
	if (!p || (rc = __mnt_fs_set_fstype_ptr(fs, p))) {
		DBG(TAB, ul_debug("tab parse error: [fstype]"));
		free(p);
		goto fail;
	}

	/* (9) source -- maybe empty string */
	if (!s || !*s) {
		DBG(TAB, ul_debug("tab parse error: [source]"));
		goto fail;
	} else if (*s == ' ' && *(s+1) == ' ') {
		if ((rc = mnt_fs_set_source(fs, ""))) {
			DBG(TAB, ul_debug("tab parse error: [empty source]"));
			goto fail;
		}
	} else {
		s = skip_separator(s);
		p = unmangle(s, &s);
		if (!p || (rc = __mnt_fs_set_source_ptr(fs, p))) {
			DBG(TAB, ul_debug("tab parse error: [regular source]"));
			free(p);
			goto fail;
		}
	}

	s = skip_separator(s);

	/* (10) fs options (fs specific) */
	fs->fs_optstr = unmangle(s, &s);
	if (!fs->fs_optstr) {
		DBG(TAB, ul_debug("tab parse error: [FS options]"));
		goto fail;
	}

	/* merge VFS and FS options to one string */
	fs->optstr = mnt_fs_strdup_options(fs);
	if (!fs->optstr) {
		rc = -ENOMEM;
		DBG(TAB, ul_debug("tab parse error: [merge VFS and FS options]"));
		goto fail;
	}

	return 0;
fail:
	if (rc == 0)
		rc = -EINVAL;
	DBG(TAB, ul_debug("tab parse error on: '%s' [rc=%d]", s, rc));
	return rc;
}

/*
 * Parses one line from utab file
 */
static int mnt_parse_utab_line(struct libmnt_fs *fs, const char *s)
{
	const char *p = s;

	assert(fs);
	assert(s);
	assert(!fs->source);
	assert(!fs->target);

	while (p && *p) {
		const char *end = NULL;

		while (*p == ' ') p++;
		if (!*p)
			break;

		if (!fs->source && !strncmp(p, "SRC=", 4)) {
			char *v = unmangle(p + 4, &end);
			if (!v)
				goto enomem;
			if (__mnt_fs_set_source_ptr(fs, v))
				free(v);

		} else if (!fs->target && !strncmp(p, "TARGET=", 7)) {
			fs->target = unmangle(p + 7, &end);
			if (!fs->target)
				goto enomem;

		} else if (!fs->root && !strncmp(p, "ROOT=", 5)) {
			fs->root = unmangle(p + 5, &end);
			if (!fs->root)
				goto enomem;

		} else if (!fs->bindsrc && !strncmp(p, "BINDSRC=", 8)) {
			fs->bindsrc = unmangle(p + 8, &end);
			if (!fs->bindsrc)
				goto enomem;

		} else if (!fs->user_optstr && !strncmp(p, "OPTS=", 5)) {
			fs->user_optstr = unmangle(p + 5, &end);
			if (!fs->user_optstr)
				goto enomem;

		} else if (!fs->attrs && !strncmp(p, "ATTRS=", 6)) {
			fs->attrs = unmangle(p + 6, &end);
			if (!fs->attrs)
				goto enomem;

		} else {
			/* unknown variable */
			while (*p && *p != ' ') p++;
		}
		if (end)
			p = end;
	}

	return 0;
enomem:
	DBG(TAB, ul_debug("utab parse error: ENOMEM"));
	return -ENOMEM;
}

/*
 * Parses one line from /proc/swaps
 */
static int mnt_parse_swaps_line(struct libmnt_fs *fs, const char *s)
{
	uint64_t num;
	int rc = 0;
	char *p;

	/* (1) source */
	p = unmangle(s, &s);
	if (p) {
		char *x = (char *) endswith(p, PATH_DELETED_SUFFIX);
		if (x && *x)
			*x = '\0';
	}
	if (!p || (rc = __mnt_fs_set_source_ptr(fs, p))) {
		DBG(TAB, ul_debug("tab parse error: [source]"));
		free(p);
		goto fail;
	}

	s = skip_separator(s);

	/* (2) type */
	fs->swaptype = unmangle(s, &s);
	if (!fs->swaptype) {
		DBG(TAB, ul_debug("tab parse error: [swaptype]"));
		goto fail;
	}

	s = skip_separator(s);

	/* (3) size */
	s = next_u64(s, &num, &rc);
	if (!s || !*s || rc) {
		DBG(TAB, ul_debug("tab parse error: [size]"));
		goto fail;
	}
	fs->size = num;

	s = skip_separator(s);

	/* (4) size */
	s = next_u64(s, &num, &rc);
	if (!s || !*s || rc) {
		DBG(TAB, ul_debug("tab parse error: [used size]"));
		goto fail;
	}
	fs->usedsize = num;

	s = skip_separator(s);

	/* (5) priority */
	s = next_s32(s, &fs->priority, &rc);
	if (rc) {
		DBG(TAB, ul_debug("tab parse error: [priority]"));
		goto fail;
	}

	mnt_fs_set_fstype(fs, "swap");
	return 0;
fail:
	if (rc == 0)
		rc = -EINVAL;
	DBG(TAB, ul_debug("tab parse error on: '%s' [rc=%d]", s, rc));
	return rc;
}


/*
 * Returns {m,fs}tab or mountinfo file format (MNT_FMT_*)
 *
 * Note that we aren't trying to guess the utab file format, because this file
 * always has to be parsed by private libmount routines with an explicitly defined
 * format.
 *
 * mountinfo: "<number> <number> ... "
 */
static int guess_table_format(const char *line)
{
	unsigned int a, b;

	DBG(TAB, ul_debug("trying to guess table type"));

	if (sscanf(line, "%u %u", &a, &b) == 2)
		return MNT_FMT_MOUNTINFO;

	if (strncmp(line, "Filename\t", 9) == 0)
		return MNT_FMT_SWAPS;

	return MNT_FMT_FSTAB;		/* fstab, mtab or /proc/mounts */
}

static int is_comment_line(const char *line)
{
	const char *p = skip_blank(line);

	if (p && (*p == '#' || *p == '\n'))
		return 1;
	return 0;
}

/* returns 1 if the last line in the @str is blank */
static int is_terminated_by_blank(const char *str)
{
	size_t sz = str ? strlen(str) : 0;
	const char *p = sz ? str + (sz - 1) : NULL;

	if (!sz || !p || *p != '\n')
		return 0;		/* empty or not terminated by '\n' */
	if (p == str)
		return 1;		/* only '\n' */
	p--;
	while (p >= str && (*p == ' ' || *p == '\t'))
		p--;
	return *p == '\n' ? 1 : 0;
}

/*
 * Reads the next line from the file.
 *
 * Returns 0 if the line is a comment
 *         1 if the line is not a comment
 *        <0 on error
 */
static int next_comment_line(struct libmnt_parser *pa, char **last)
{
	if (getline(&pa->buf, &pa->bufsiz, pa->f) < 0)
		return feof(pa->f) ? 1 : -errno;

	pa->line++;
	*last = strchr(pa->buf, '\n');

	return is_comment_line(pa->buf) ? 0 : 1;
}

static int append_comment(struct libmnt_table *tb,
			  struct libmnt_fs *fs,
			  const char *comm,
			  int eof)
{
	int rc, intro = mnt_table_get_nents(tb) == 0;

	if (intro && is_terminated_by_blank(mnt_table_get_intro_comment(tb)))
		intro = 0;

	DBG(TAB, ul_debugobj(tb, "appending %s comment",
			intro ? "intro" :
			eof ? "trailing" : "fs"));
	if (intro)
		rc = mnt_table_append_intro_comment(tb, comm);
	else if (eof) {
		rc = mnt_table_set_trailing_comment(tb,
				mnt_fs_get_comment(fs));
		if (!rc)
			rc = mnt_table_append_trailing_comment(tb, comm);
		if (!rc)
			rc = mnt_fs_set_comment(fs, NULL);
	} else
		rc = mnt_fs_append_comment(fs, comm);
	return rc;
}

/*
 * Read and parse the next line from {fs,m}tab or mountinfo
 */
static int mnt_table_parse_next(struct libmnt_parser *pa,
				struct libmnt_table *tb,
				struct libmnt_fs *fs)
{
	char *s;
	int rc;

	assert(tb);
	assert(pa);
	assert(fs);

	/* read the next non-blank non-comment line */
next_line:
	do {
		if (getline(&pa->buf, &pa->bufsiz, pa->f) < 0)
			return -EINVAL;
		pa->line++;
		s = strchr(pa->buf, '\n');
		if (!s) {
			/* Missing final newline?  Otherwise an extremely */
			/* long line - assume file was corrupted */
			if (feof(pa->f)) {
				DBG(TAB, ul_debugobj(tb,
					"%s: no final newline",	pa->filename));
				s = strchr(pa->buf, '\0');
			} else {
				DBG(TAB, ul_debugobj(tb,
					"%s:%zu: missing newline at line",
					pa->filename, pa->line));
				goto err;
			}
		}

		/* comments parser */
		if (tb->comms
		    && (tb->fmt == MNT_FMT_GUESS || tb->fmt == MNT_FMT_FSTAB)
		    && is_comment_line(pa->buf)) {
			do {
				rc = append_comment(tb, fs, pa->buf, feof(pa->f));
				if (!rc)
					rc = next_comment_line(pa, &s);
			} while (rc == 0);

			if (rc == 1 && feof(pa->f))
				rc = append_comment(tb, fs, NULL, 1);
			if (rc < 0)
				return rc;

		}

		*s = '\0';
		if (--s >= pa->buf && *s == '\r')
			*s = '\0';
		s = (char *) skip_blank(pa->buf);
	} while (*s == '\0' || *s == '#');

	if (tb->fmt == MNT_FMT_GUESS) {
		tb->fmt = guess_table_format(s);
		if (tb->fmt == MNT_FMT_SWAPS)
			goto next_line;			/* skip swap header */
	}

	switch (tb->fmt) {
	case MNT_FMT_FSTAB:
		rc = mnt_parse_table_line(fs, s);
		break;
	case MNT_FMT_MOUNTINFO:
		rc = mnt_parse_mountinfo_line(fs, s);
		break;
	case MNT_FMT_UTAB:
		rc = mnt_parse_utab_line(fs, s);
		break;
	case MNT_FMT_SWAPS:
		if (strncmp(s, "Filename\t", 9) == 0)
			goto next_line;			/* skip swap header */
		rc = mnt_parse_swaps_line(fs, s);
		break;
	default:
		rc = -1;	/* unknown format */
		break;
	}

	if (rc == 0)
		return 0;
err:
	DBG(TAB, ul_debugobj(tb, "%s:%zu: %s parse error", pa->filename, pa->line,
				tb->fmt == MNT_FMT_MOUNTINFO ? "mountinfo" :
				tb->fmt == MNT_FMT_SWAPS ? "swaps" :
				tb->fmt == MNT_FMT_FSTAB ? "tab" : "utab"));

	/* by default all errors are recoverable, otherwise behavior depends on
	 * the errcb() function. See mnt_table_set_parser_errcb().
	 */
	return tb->errcb ? tb->errcb(tb, pa->filename, pa->line) : 1;
}

static pid_t path_to_tid(const char *filename)
{
	char *path = mnt_resolve_path(filename, NULL);
	char *p, *end = NULL;
	pid_t tid = 0;

	if (!path)
		goto done;
	p = strrchr(path, '/');
	if (!p)
		goto done;
	*p = '\0';
	p = strrchr(path, '/');
	if (!p)
		goto done;
	p++;

	errno = 0;
	tid = strtol(p, &end, 10);
	if (errno || p == end || (end && *end)) {
		tid = 0;
		goto done;
	}
	DBG(TAB, ul_debug("TID for %s is %d", filename, tid));
done:
	free(path);
	return tid;
}

static int kernel_fs_postparse(struct libmnt_table *tb,
			       struct libmnt_fs *fs,
			       struct libmnt_parser *pa)
{
	int rc = 0;

	/* This is a filesystem description from /proc, so we're in some process
	 * namespace. Let's remember the process PID.
	 */
	if (pa->filename && pa->tid == 0)
		pa->tid = path_to_tid(pa->filename);

	fs->tid = pa->tid;

	/*
	 * Convert obscure /dev/root to something more usable
	 */
	if (!pa->has_root_fs) {
		const char *src = mnt_fs_get_srcpath(fs);

		if (src && strcmp(src, "/dev/root") == 0) {
			char *real = NULL;

			rc = mnt_guess_system_root(mnt_fs_get_devno(fs), tb->cache, &real);
			if (rc < 0)
				return rc;

			if (rc == 0 && real) {
				DBG(TAB, ul_debugobj(tb, "canonical root FS: %s", real));
				rc = __mnt_fs_set_source_ptr(fs, real);

			} else if (rc == 1) {
				/* mnt_guess_system_root() returns 1 if not
				 * able to convert to the real devname; ignore
				 * this problem */
				rc = 0;
			}
			pa->has_root_fs = 1;
		}
	}

	return rc;
}

static int parse_fsinfo_init_fs(
			struct libmnt_table *tb,
			struct libmnt_fs *fs,
			unsigned int id,
			struct libmnt_fs *parent,
			struct libmnt_parser *pa)
{
	int rc = 0;

	fs->flags |= MNT_FS_KERNEL;
	fs->id = id;
	fs->parent = parent ? parent->id : 0;

	mnt_fs_enable_fsinfo(fs, 1);

	if (tb->fltrcb && tb->fltrcb(fs, tb->fltrcb_data))
		rc = 1;	/* filtered out by callback... */
	if (!rc)
		rc = kernel_fs_postparse(tb, fs, pa);
	return rc;
}


#ifdef USE_LIBMOUNT_SUPPORT_FSINFO
static int table_parse_fetch_chldren(struct libmnt_table *tb,
				     unsigned int id, struct libmnt_fs *parent,
				     struct libmnt_parser *pa)
{
	struct libmnt_fs *fs = NULL;
	struct fsinfo_mount_child *mounts = NULL;
	size_t i, count;
	int rc = 0;

	assert(id);

	/* add root fs */
	if (!parent) {
		DBG(TAB, ul_debugobj(tb, "fsinfo: add root FS"));
		parent = mnt_new_fs();
		if (!parent) {
			rc = -ENOMEM;
			goto out;
		}
		rc = parse_fsinfo_init_fs(tb, parent, id, NULL, pa);
		if (!rc)
			rc = mnt_table_add_fs(tb, parent);
		if (rc)
			goto out;
		pa->has_root_fs = 1;
	}

	/* children list */
	rc = mnt_fsinfo_get_children(id, &mounts, &count);
	if (rc != 0)
		goto out;
	if (!count)
		goto out;

	/* add childern to the table */
	for (i = 0; i < count; i++) {
		if (!fs) {
			fs = mnt_new_fs();
			if (!fs) {
				rc = -ENOMEM;
				goto out;
			}
		}
		rc = parse_fsinfo_init_fs(tb, fs, mounts[i].mnt_id, parent, pa);
		if (!rc)
			rc = mnt_table_add_fs(tb, fs);
		if (!rc) {
			rc = table_parse_fetch_chldren(tb, mnt_fs_get_id(fs), fs, pa);
			if (rc)
				mnt_table_remove_fs(tb, fs);
		}
		if (rc) {
			if (rc > 0) {
				mnt_reset_fs(fs);
				assert(fs->refcount == 1);
				continue;	/* recoverable error, reuse fs*/
			}

			mnt_unref_fs(fs);
			goto out;		/* fatal error */
		}

		mnt_unref_fs(fs);
		fs = NULL;
	}
out:
	free(mounts);
	return rc;
}

static int __table_parse_fsinfo(struct libmnt_table *tb)
{
	unsigned int id;
	int rc;
	struct libmnt_parser pa = { .filename = _PATH_PROC_MOUNTINFO };

	DBG(TAB, ul_debugobj(tb, "fsinfo parse: start parsing [entries=%d, filter=%s]",
				mnt_table_get_nents(tb), tb->fltrcb ? "yes" : "not"));

	rc = mnt_get_target_id("/", &id, AT_NO_AUTOMOUNT);
	if (rc < 0)
		goto err;

	rc = table_parse_fetch_chldren(tb, id, NULL, &pa);
	if (rc < 0)
		goto err;

	DBG(TAB, ul_debugobj(tb, "fsinfo parse : stop parsing (%d entries)",
				mnt_table_get_nents(tb)));
	return 0;
err:
	DBG(TAB, ul_debugobj(tb, "fsinfo parse: error (rc=%d)", rc));
	return rc;
}
#endif /* USE_LIBMOUNT_SUPPORT_FSINFO */

/**
 * mnt_table_parse_fsinfo:
 * @tb: libmnt_table instance
 *
 * Read mount table by fsinfo() kernel interface. Note that all filesystems in the
 * table have enabled on-demand fsinfo().
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_table_parse_fsinfo(struct libmnt_table *tb)
{
	int rc = -ENOSYS;

	assert(tb);
#ifdef USE_LIBMOUNT_SUPPORT_FSINFO
	if (mnt_has_fsinfo())
		rc = __table_parse_fsinfo(tb);
#endif
	return rc;
}

static int __table_parse_stream(struct libmnt_table *tb, FILE *f, const char *filename)
{
	int rc = -1;
	int flags = 0;
	struct libmnt_fs *fs = NULL;
	struct libmnt_parser pa = { .line = 0 };

	assert(tb);
	assert(f);
	assert(filename);

	DBG(TAB, ul_debugobj(tb, "%s: start parsing [entries=%d, filter=%s]",
				filename, mnt_table_get_nents(tb),
				tb->fltrcb ? "yes" : "not"));

	pa.filename = filename;
	pa.f = f;

	/* necessary for /proc/mounts only, the /proc/self/mountinfo
	 * parser sets the flag properly
	 */
	if (filename && strcmp(filename, _PATH_PROC_MOUNTS) == 0)
		flags = MNT_FS_KERNEL;

	while (!feof(f)) {
		if (!fs) {
			fs = mnt_new_fs();
			if (!fs)
				goto err;
		}

		rc = mnt_table_parse_next(&pa, tb, fs);

		if (!rc && tb->fltrcb && tb->fltrcb(fs, tb->fltrcb_data))
			rc = 1;	/* filtered out by callback... */

		if (!rc) {
			rc = mnt_table_add_fs(tb, fs);
			fs->flags |= flags;

			if (rc == 0 && tb->fmt == MNT_FMT_MOUNTINFO) {
				rc = kernel_fs_postparse(tb, fs, &pa);
				if (rc)
					mnt_table_remove_fs(tb, fs);
			}
		}

		if (rc) {
			if (rc > 0) {
				mnt_reset_fs(fs);
				assert(fs->refcount == 1);
				continue;	/* recoverable error, reuse fs*/
			}

			mnt_unref_fs(fs);
			if (feof(f))
				break;
			goto err;		/* fatal error */
		}
		mnt_unref_fs(fs);
		fs = NULL;
	}

	DBG(TAB, ul_debugobj(tb, "%s: stop parsing (%d entries)",
				filename, mnt_table_get_nents(tb)));
	parser_cleanup(&pa);
	return 0;
err:
	DBG(TAB, ul_debugobj(tb, "%s: parse error (rc=%d)", filename, rc));
	parser_cleanup(&pa);
	return rc;
}

/**
 * mnt_table_parse_stream:
 * @tb: tab pointer
 * @f: file stream
 * @filename: filename used for debug and error messages
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_table_parse_stream(struct libmnt_table *tb, FILE *f, const char *filename)
{
	int fd, rc;
	FILE *memf = NULL;
	char *membuf = NULL;

	/*
	 * For /proc/#/{mountinfo,mount} we read all file to memory and use it
	 * as memory stream. For more details see mnt_read_procfs_file().
	 */
	if ((fd = fileno(f)) >= 0
	    && (tb->fmt == MNT_FMT_GUESS ||
		tb->fmt == MNT_FMT_MOUNTINFO ||
		tb->fmt == MNT_FMT_MTAB)
	    && is_procfs_fd(fd)
	    && (memf = mnt_get_procfs_memstream(fd, &membuf))) {

		rc = __table_parse_stream(tb, memf, filename);
		fclose(memf);
		free(membuf);
	} else
		rc = __table_parse_stream(tb, f, filename);

	return rc;
}

/**
 * mnt_table_parse_file:
 * @tb: tab pointer
 * @filename: file
 *
 * Parses the whole table (e.g. /etc/fstab) and appends new records to the @tab.
 *
 * The libmount parser ignores broken (syntax error) lines, these lines are
 * reported to the caller by the errcb() function (see mnt_table_set_parser_errcb()).
 *
 * Returns: 0 on success, negative number in case of error.
 */
int mnt_table_parse_file(struct libmnt_table *tb, const char *filename)
{
	FILE *f;
	int rc, fd = -1;

	if (!filename || !tb)
		return -EINVAL;

	/*
	 * Try to use read()+poll() to realiably read all
	 * /proc/#/{mount,mountinfo} file to memory
	 */
	if (tb->fmt != MNT_FMT_SWAPS
	    && strncmp(filename, "/proc/", 6) == 0) {

		FILE *memf;
		char *membuf = NULL;

		fd = open(filename, O_RDONLY|O_CLOEXEC);
		if (fd < 0) {
			rc = -errno;
			goto done;
		}
		memf = mnt_get_procfs_memstream(fd, &membuf);
		if (memf) {
			rc = __table_parse_stream(tb, memf, filename);

			fclose(memf);
			free(membuf);
			close(fd);
			goto done;
		}
		/* else fallback to fopen/fdopen() */
	}

	if (fd >= 0)
		f = fdopen(fd, "r" UL_CLOEXECSTR);
	else
		f = fopen(filename, "r" UL_CLOEXECSTR);

	if (f) {
		rc = __table_parse_stream(tb, f, filename);
		fclose(f);
	} else
		rc = -errno;
done:
	DBG(TAB, ul_debugobj(tb, "parsing done [filename=%s, rc=%d]", filename, rc));
	return rc;
}

static int mnt_table_parse_dir_filter(const struct dirent *d)
{
	size_t namesz;

#ifdef _DIRENT_HAVE_D_TYPE
	if (d->d_type != DT_UNKNOWN && d->d_type != DT_REG &&
	    d->d_type != DT_LNK)
		return 0;
#endif
	if (*d->d_name == '.')
		return 0;

#define MNT_MNTTABDIR_EXTSIZ	(sizeof(MNT_MNTTABDIR_EXT) - 1)

	namesz = strlen(d->d_name);
	if (!namesz || namesz < MNT_MNTTABDIR_EXTSIZ + 1 ||
	    strcmp(d->d_name + (namesz - MNT_MNTTABDIR_EXTSIZ),
		   MNT_MNTTABDIR_EXT))
		return 0;

	/* Accept this */
	return 1;
}

#ifdef HAVE_SCANDIRAT
static int __mnt_table_parse_dir(struct libmnt_table *tb, const char *dirname)
{
	int n = 0, i;
	int dd;
	struct dirent **namelist = NULL;

	dd = open(dirname, O_RDONLY|O_CLOEXEC|O_DIRECTORY);
	if (dd < 0)
	        return -errno;

	n = scandirat(dd, ".", &namelist, mnt_table_parse_dir_filter, versionsort);
	if (n <= 0) {
	        close(dd);
	        return 0;
	}

	for (i = 0; i < n; i++) {
		struct dirent *d = namelist[i];
		struct stat st;
		FILE *f;

		if (fstatat(dd, d->d_name, &st, 0) ||
		    !S_ISREG(st.st_mode))
			continue;

		f = fopen_at(dd, d->d_name, O_RDONLY|O_CLOEXEC, "r" UL_CLOEXECSTR);
		if (f) {
			__table_parse_stream(tb, f, d->d_name);
			fclose(f);
		}
	}

	for (i = 0; i < n; i++)
		free(namelist[i]);
	free(namelist);
	close(dd);
	return 0;
}
#else
static int __mnt_table_parse_dir(struct libmnt_table *tb, const char *dirname)
{
	int n = 0, i, r = 0;
	DIR *dir = NULL;
	struct dirent **namelist = NULL;

	n = scandir(dirname, &namelist, mnt_table_parse_dir_filter, versionsort);
	if (n <= 0)
		return 0;

	/* let's use "at" functions rather than playing crazy games with paths... */
	dir = opendir(dirname);
	if (!dir) {
		r = -errno;
		goto out;
	}

	for (i = 0; i < n; i++) {
		struct dirent *d = namelist[i];
		struct stat st;
		FILE *f;

		if (fstatat(dirfd(dir), d->d_name, &st, 0) ||
		    !S_ISREG(st.st_mode))
			continue;

		f = fopen_at(dirfd(dir), d->d_name,
				O_RDONLY|O_CLOEXEC, "r" UL_CLOEXECSTR);
		if (f) {
			__table_parse_stream(tb, f, d->d_name);
			fclose(f);
		}
	}

out:
	for (i = 0; i < n; i++)
		free(namelist[i]);
	free(namelist);
	if (dir)
		closedir(dir);
	return r;
}
#endif

/**
 * mnt_table_parse_dir:
 * @tb: mount table
 * @dirname: directory
 *
 * The directory:
 *	- files are sorted by strverscmp(3)
 *	- files that start with "." are ignored (e.g. ".10foo.fstab")
 *	- files without the ".fstab" extension are ignored
 *
 * Returns: 0 on success or negative number in case of error.
 */
int mnt_table_parse_dir(struct libmnt_table *tb, const char *dirname)
{
	return __mnt_table_parse_dir(tb, dirname);
}

struct libmnt_table *__mnt_new_table_from_file(const char *filename, int fmt, int empty_for_enoent)
{
	struct libmnt_table *tb;
	struct stat st;

	if (!filename)
		return NULL;
	if (stat(filename, &st))
		return empty_for_enoent ? mnt_new_table() : NULL;

	tb = mnt_new_table();
	if (tb) {
		DBG(TAB, ul_debugobj(tb, "new tab for file: %s", filename));
		tb->fmt = fmt;
		if (mnt_table_parse_file(tb, filename) != 0) {
			mnt_unref_table(tb);
			tb = NULL;
		}
	}
	return tb;
}

/**
 * mnt_new_table_from_file:
 * @filename: /etc/{m,fs}tab or /proc/self/mountinfo path
 *
 * Same as mnt_new_table() + mnt_table_parse_file(). Use this function for private
 * files only. This function does not allow using the error callback, so you
 * cannot provide any feedback to end-users about broken records in files (e.g.
 * fstab).
 *
 * Returns: newly allocated tab on success and NULL in case of error.
 */
struct libmnt_table *mnt_new_table_from_file(const char *filename)
{
	if (!filename)
		return NULL;

	return __mnt_new_table_from_file(filename, MNT_FMT_GUESS, 0);
}

/**
 * mnt_new_table_from_dir
 * @dirname: directory with *.fstab files
 *
 * Returns: newly allocated tab on success and NULL in case of error.
 */
struct libmnt_table *mnt_new_table_from_dir(const char *dirname)
{
	struct libmnt_table *tb;

	if (!dirname)
		return NULL;
	tb = mnt_new_table();
	if (tb && mnt_table_parse_dir(tb, dirname) != 0) {
		mnt_unref_table(tb);
		tb = NULL;
	}
	return tb;
}

/**
 * mnt_table_set_parser_errcb:
 * @tb: pointer to table
 * @cb: pointer to callback function
 *
 * The error callback function is called by table parser (mnt_table_parse_file())
 * in case of a syntax error. The callback function could be used for error
 * evaluation, libmount will continue/stop parsing according to callback return
 * codes:
 *
 *   <0  : fatal error (abort parsing)
 *    0	 : success (parsing continues)
 *   >0  : recoverable error (the line is ignored, parsing continues).
 *
 * Returns: 0 on success or negative number in case of error.
 */
int mnt_table_set_parser_errcb(struct libmnt_table *tb,
		int (*cb)(struct libmnt_table *tb, const char *filename, int line))
{
	if (!tb)
		return -EINVAL;
	tb->errcb = cb;
	return 0;
}

/*
 * Filter out entries during tab file parsing. If @cb returns 1, then the entry
 * is ignored.
 */
int mnt_table_set_parser_fltrcb(struct libmnt_table *tb,
		int (*cb)(struct libmnt_fs *, void *),
		void *data)
{
	if (!tb)
		return -EINVAL;

	DBG(TAB, ul_debugobj(tb, "%s table parser filter", cb ? "set" : "unset"));
	tb->fltrcb = cb;
	tb->fltrcb_data = data;
	return 0;
}

/**
 * mnt_table_parse_swaps:
 * @tb: table
 * @filename: overwrites default (/proc/swaps or $LIBMOUNT_SWAPS) or NULL
 *
 * This function parses /proc/swaps and appends new lines to the @tab.
 *
 * See also mnt_table_set_parser_errcb().
 *
 * Returns: 0 on success or negative number in case of error.
 */
int mnt_table_parse_swaps(struct libmnt_table *tb, const char *filename)
{
	if (!tb)
		return -EINVAL;
	if (!filename) {
		filename = mnt_get_swaps_path();
		if (!filename)
			return -EINVAL;
	}

	tb->fmt = MNT_FMT_SWAPS;

	return mnt_table_parse_file(tb, filename);
}

/**
 * mnt_table_parse_fstab:
 * @tb: table
 * @filename: overwrites default (/etc/fstab or $LIBMOUNT_FSTAB) or NULL
 *
 * This function parses /etc/fstab and appends new lines to the @tab. If the
 * @filename is a directory, then mnt_table_parse_dir() is called.
 *
 * See also mnt_table_set_parser_errcb().
 *
 * Returns: 0 on success or negative number in case of error.
 */
int mnt_table_parse_fstab(struct libmnt_table *tb, const char *filename)
{
	struct stat st;
	int rc = 0;

	if (!tb)
		return -EINVAL;
	if (!filename)
		filename = mnt_get_fstab_path();
	if (!filename)
		return -EINVAL;
	if (stat(filename, &st) != 0)
		return -errno;

	tb->fmt = MNT_FMT_FSTAB;

	if (S_ISREG(st.st_mode))
		rc = mnt_table_parse_file(tb, filename);
	else if (S_ISDIR(st.st_mode))
		rc = mnt_table_parse_dir(tb, filename);
	else
		rc = -EINVAL;

	return rc;
}

/*
 * This function uses @uf to find a corresponding record in @tb, then the record
 * from @tb is updated (user specific mount options are added).
 *
 * Note that @uf must contain only user specific mount options instead of
 * VFS options (note that FS options are ignored).
 *
 * Returns modified filesystem (from @tb) or NULL.
 */
static struct libmnt_fs *mnt_table_merge_utab_fs(struct libmnt_table *tb, struct libmnt_fs *uf)
{
	struct libmnt_fs *fs;
	const char *src, *target, *root;

	if (!tb || !uf)
		return NULL;

	DBG(TAB, ul_debugobj(tb, "merging user fs"));

	src = mnt_fs_get_srcpath(uf);
	target = mnt_fs_get_target(uf);
	root = mnt_fs_get_root(uf);

	if (!src || !target || !root)
		return NULL;

	fs = mnt_table_find_triplet(tb, src, target, root, MNT_ITER_BACKWARD);
	if (fs) {
		DBG(TAB, ul_debugobj(tb, "found fs -- appending user optstr"));
		mnt_fs_merge_utab(fs, uf);
		DBG(TAB, ul_debugobj(tb, "found fs:"));
		DBG(TAB, mnt_fs_print_debug(fs, stderr));
	}
	return fs;
}

/* default filename is /proc/self/mountinfo
 */
int __mnt_table_parse_mtab(struct libmnt_table *tb, const char *filename,
			   struct libmnt_table *u_tb)
{
	int rc = 0, priv_utab = 0;
	int explicit_file = filename ? 1 : 0;

	assert(tb);

	if (filename)
		DBG(TAB, ul_debugobj(tb, "%s requested as mtab", filename));

#ifdef USE_LIBMOUNT_SUPPORT_MTAB
	if (mnt_has_regular_mtab(&filename, NULL)) {

		DBG(TAB, ul_debugobj(tb, "force mtab usage [filename=%s]", filename));

		rc = mnt_table_parse_file(tb, filename);

		/*
		 * If @filename forces us to read from /proc then also read
		 * utab file to merge userspace mount options.
		 */
		if (rc == 0 && is_mountinfo(tb))
			goto read_utab;

		if (!rc)
			return 0;
		filename = NULL;	/* failed */
	} else
		filename = NULL;	/* mtab useless */
#endif

	if (!filename || strcmp(filename, _PATH_PROC_MOUNTINFO) == 0) {
		filename = _PATH_PROC_MOUNTINFO;
		tb->fmt = MNT_FMT_MOUNTINFO;
		DBG(TAB, ul_debugobj(tb, "mtab parse: #1 read mountinfo"));
	} else
		tb->fmt = MNT_FMT_GUESS;

	rc = mnt_table_parse_file(tb, filename);
	if (rc) {
		if (explicit_file)
			return rc;

		/* hmm, old kernel? ...try /proc/mounts */
		tb->fmt = MNT_FMT_MTAB;
		return mnt_table_parse_file(tb, _PATH_PROC_MOUNTS);
	}

	if (!is_mountinfo(tb))
		return 0;
#ifdef USE_LIBMOUNT_SUPPORT_MTAB
read_utab:
#endif
	DBG(TAB, ul_debugobj(tb, "mtab parse: #2 read utab"));

	if (mnt_table_get_nents(tb) == 0)
		return 0;			/* empty, ignore utab */
	/*
	 * try to read the user specific information from /run/mount/utabs
	 */
	if (!u_tb) {
		const char *utab = mnt_get_utab_path();

		if (!utab || is_file_empty(utab))
			return 0;

		u_tb = mnt_new_table();
		if (!u_tb)
			return -ENOMEM;

		u_tb->fmt = MNT_FMT_UTAB;
		mnt_table_set_parser_fltrcb(u_tb, tb->fltrcb, tb->fltrcb_data);

		rc = mnt_table_parse_file(u_tb, utab);
		priv_utab = 1;
	}

	DBG(TAB, ul_debugobj(tb, "mtab parse: #3 merge utab"));

	if (rc == 0) {
		struct libmnt_fs *u_fs;
		struct libmnt_iter itr;

		mnt_reset_iter(&itr, MNT_ITER_BACKWARD);

		/*  merge user options into mountinfo from the kernel */
		while (mnt_table_next_fs(u_tb, &itr, &u_fs) == 0)
			mnt_table_merge_utab_fs(tb, u_fs);
	}

	if (priv_utab)
		mnt_unref_table(u_tb);
	return 0;
}
/**
 * mnt_table_parse_mtab:
 * @tb: table
 * @filename: overwrites default or NULL
 *
 * The default filename is /proc/self/mountinfo. If the mount table is a
 * mountinfo file then /run/mount/utabs is parsed too and both files are merged
 * to the one libmnt_table.
 *
 * If libmount is compiled with classic mtab file support, and the /etc/mtab is
 * a regular file then this file is parsed.
 *
 * It's strongly recommended to use NULL as a @filename to keep code portable.
 *
 * See also mnt_table_set_parser_errcb().
 *
 * Returns: 0 on success or negative number in case of error.
 */
int mnt_table_parse_mtab(struct libmnt_table *tb, const char *filename)
{
	return __mnt_table_parse_mtab(tb, filename, NULL);
}
