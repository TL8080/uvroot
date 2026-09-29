/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of uvroot.
 *
 * Copyright (C) 2026 uvroot Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 */

/*
 * Raw image block backend ("img", "raw", "file").
 *
 * Exposes a local disk image as a byte-addressable device.  A filesystem
 * driver (see fs_ext2.c) is stacked on top of it to present it as a
 * directory tree, so the image behaves like a real disk: permissions
 * live in the image itself, not in a .uvroot-vperm database.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cli/note.h"
#include "extension/netfs/netfs.h"

static const char *const img_schemes[] = { "img", "raw", "file", NULL };

const char *netfs_img_path(const char *url)
{
    if (strncasecmp(url, "img://", 6) == 0)
	return url + 6;
    if (strncasecmp(url, "raw://", 6) == 0)
	return url + 6;
    if (strncasecmp(url, "file://", 7) == 0)
	return url + 7;
    return url;
}

static void img_close(NetfsMount *mount)
{
    int *fd = mount->backend_data;

    if (fd != NULL) {
	if (*fd >= 0)
	    close(*fd);
	TALLOC_FREE(fd);
    }
    mount->backend_data = NULL;
}

static int img_init(NetfsMount *mount, const char *url)
{
    const char *path = netfs_img_path(url);
    int *fd;

    if (path[0] == '\0') {
	note(NULL, ERROR, USER, "netfs: no image path given for %s", url);
	return -EINVAL;
    }

    fd = talloc_zero(mount, int);
    if (fd == NULL)
	return -ENOMEM;

    *fd = open(path, O_RDWR | O_CLOEXEC);
    if (*fd < 0)
	*fd = open(path, O_RDONLY | O_CLOEXEC);	/* read-only image */
    if (*fd < 0) {
	int status = -errno;

	note(NULL, ERROR, USER, "netfs: cannot open image \"%s\": %s",
	     path, strerror(-status));
	TALLOC_FREE(fd);
	return status;
    }

    {
	/*
	 * Two containers sharing a writable image would each write back
	 * their own block and inode bitmaps and corrupt it, so only one
	 * may open it at a time.  Read-only images may be shared.
	 */
	bool writable = (fcntl(*fd, F_GETFL) & O_ACCMODE) != O_RDONLY;
	int status = netfs_lock_image(*fd, writable, path);

	if (status < 0) {
	    close(*fd);
	    TALLOC_FREE(fd);
	    return status;
	}
    }

    mount->backend_data = fd;
    return 0;
}

static int img_size(NetfsMount *mount, uint64_t *bytes)
{
    struct stat st;
    int fd = *(int *) mount->backend_data;

    if (fstat(fd, &st) < 0)
	return -errno;

    *bytes = (uint64_t) st.st_size;
    return 0;
}

static ssize_t img_pread(NetfsMount *mount, void *buffer, size_t count,
			 uint64_t offset)
{
    int fd = *(int *) mount->backend_data;
    ssize_t got = pread(fd, buffer, count, (off_t) offset);

    return got < 0 ? -errno : got;
}

static ssize_t img_pwrite(NetfsMount *mount, const void *buffer, size_t count,
			  uint64_t offset)
{
    int fd = *(int *) mount->backend_data;
    ssize_t put = pwrite(fd, buffer, count, (off_t) offset);

    return put < 0 ? -errno : put;
}

static int img_flush(NetfsMount *mount)
{
    int fd = *(int *) mount->backend_data;

    return fsync(fd) < 0 ? -errno : 0;
}

const NetfsBackend netfs_backend_img = {
    .name = "img",
    .schemes = img_schemes,
    .kind = NETFS_KIND_BLOCK,
    .implemented = true,
    .block = {
	    .open = NULL,	/* opened by img_init() */
	    .close = img_close,
	    .size = img_size,
	    .pread = img_pread,
	    .pwrite = img_pwrite,
	    .flush = img_flush,
	    },
    .init = img_init,
    .fini = img_close,
};
