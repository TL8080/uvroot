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
 * NBD (Network Block Device) block backend.
 *
 * An NBD export is a byte-addressable device, so a filesystem driver
 * (fs_ext2.c) is stacked on top of it to expose a directory tree.  The
 * transfer is handled by libnbd, which is resolved with dlopen() at run
 * time: uvroot keeps no link-time dependency and keeps working on
 * unrooted Android/Termux systems where libnbd is missing.  When the
 * library is absent the backend reports that it is not available.
 *
 * Accepted URIs are whatever libnbd understands, for instance:
 *
 *     nbd://host:10809/export
 *     nbd+unix:///export?socket=/run/nbd.sock
 *     nbds://host/export                  (TLS)
 *
 * A bare "host[:port][/export]" is accepted too and read as nbd://.
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "build.h"
#include "cli/note.h"
#include "extension/netfs/netfs.h"

static const char *const nbd_schemes[] = {
    "nbd", "nbds", "nbd+unix", "nbd+tcp", "nbd+vsock", NULL
};

#ifdef HAVE_LIBNBD

#include <libnbd.h>
#include <dlfcn.h>

typedef struct NbdApi {
    void *handle;
    struct nbd_handle *(*create) (void);
    void (*close) (struct nbd_handle * handle);
    const char *(*get_error) (void);
    int (*get_errno) (void);
    int (*set_export_name) (struct nbd_handle * handle, const char *name);
    int (*set_uri_allow_tls) (struct nbd_handle * handle, int tls);
    int (*set_uri_allow_local_file) (struct nbd_handle * handle, int allow);
    int (*set_request_block_size) (struct nbd_handle * handle, int request);
    int (*set_pread_initialize) (struct nbd_handle * handle, int request);
    int (*connect_uri) (struct nbd_handle * handle, const char *uri);
    int (*connect_unix) (struct nbd_handle * handle, const char *path);
    int64_t(*get_size) (struct nbd_handle * handle);
    int (*can_flush) (struct nbd_handle * handle);
    int (*pread) (struct nbd_handle * handle, void *buffer, size_t count,
		  uint64_t offset, uint32_t flags);
    int (*pwrite) (struct nbd_handle * handle, const void *buffer,
		   size_t count, uint64_t offset, uint32_t flags);
    int (*flush) (struct nbd_handle * handle, uint32_t flags);
    int (*shutdown) (struct nbd_handle * handle, uint32_t flags);
} NbdApi;

static NbdApi nbd_api;
static bool nbd_api_ready = false;

typedef struct NbdData {
    struct nbd_handle *handle;
    uint64_t size;
    bool can_flush;
} NbdData;

static void *nbd_open_library(const char *const *candidates)
{
    size_t i;
    void *handle;

    for (i = 0; candidates[i] != NULL; i++) {
	if (candidates[i][0] == '\0')
	    continue;
	handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
	if (handle != NULL)
	    return handle;
    }

    return NULL;
}

static bool load_nbd_api(void)
{
    static const char *candidates[8];
    static char prefix_libnbd[PATH_MAX];
    const char *environment;
    const char *prefix;
    void *handle;
    size_t i = 0;

    if (nbd_api_ready)
	return true;

    environment = getenv("UVROOT_NETFS_LIBNBD");
    if (environment != NULL && environment[0] != '\0')
	candidates[i++] = environment;

    prefix = getenv("PREFIX");
    if (prefix != NULL && prefix[0] != '\0') {
	snprintf(prefix_libnbd, sizeof(prefix_libnbd), "%s/lib/libnbd.so",
		 prefix);
	candidates[i++] = prefix_libnbd;
    }

    candidates[i++] = "libnbd.so.0";
    candidates[i++] = "libnbd.so";
    candidates[i++] = NULL;

    handle = nbd_open_library(candidates);
    if (handle == NULL) {
	VERBOSE(NULL, 1, "netfs: libnbd not found: %s", dlerror());
	return false;
    }

    memset(&nbd_api, 0, sizeof(nbd_api));
    nbd_api.handle = handle;

#define RESOLVE(field, symbol)						\
    do {								\
	*(void **) (&nbd_api.field) = dlsym(handle, symbol);		\
	if (nbd_api.field == NULL) {					\
	    VERBOSE(NULL, 1, "netfs: %s not found in libnbd", symbol);	\
	    dlclose(handle);						\
	    nbd_api.handle = NULL;					\
	    return false;						\
	}								\
    } while (0)

    RESOLVE(create, "nbd_create");
    RESOLVE(close, "nbd_close");
    RESOLVE(get_error, "nbd_get_error");
    RESOLVE(get_errno, "nbd_get_errno");
    RESOLVE(set_export_name, "nbd_set_export_name");
    RESOLVE(set_uri_allow_tls, "nbd_set_uri_allow_tls");
    RESOLVE(set_uri_allow_local_file, "nbd_set_uri_allow_local_file");
    RESOLVE(set_request_block_size, "nbd_set_request_block_size");
    RESOLVE(set_pread_initialize, "nbd_set_pread_initialize");
    RESOLVE(connect_uri, "nbd_connect_uri");
    RESOLVE(connect_unix, "nbd_connect_unix");
    RESOLVE(get_size, "nbd_get_size");
    RESOLVE(can_flush, "nbd_can_flush");
    RESOLVE(pread, "nbd_pread");
    RESOLVE(pwrite, "nbd_pwrite");
    RESOLVE(flush, "nbd_flush");
    RESOLVE(shutdown, "nbd_shutdown");
#undef RESOLVE

    nbd_api_ready = true;
    return true;
}

/*
 * Build the URI libnbd expects.  A specification without any
 * "scheme://" is read as a plain TCP one so that
 *
 *     --nbd=/mnt/disk:127.0.0.1:10809/export
 *
 * works as well as the fully qualified form.
 */
static char *nbd_normalize_uri(const char *url)
{
    if (strstr(url, "://") != NULL)
	return strdup(url);

    {
	size_t length = strlen("nbd://") + strlen(url) + 1;
	char *result = malloc(length);

	if (result != NULL)
	    snprintf(result, length, "nbd://%s", url);
	return result;
    }
}

static void nbd_mount_close(NetfsMount *mount)
{
    NbdData *data = mount->backend_data;

    if (data != NULL) {
	if (data->handle != NULL) {
	    nbd_api.shutdown(data->handle, 0);
	    nbd_api.close(data->handle);
	}
	TALLOC_FREE(data);
    }
    mount->backend_data = NULL;
}

static int nbd_mount_init(NetfsMount *mount, const char *url)
{
    NbdData *data;
    char *uri;
    int status;

    if (!load_nbd_api())
	return -ENOSYS;

    data = talloc_zero(mount, NbdData);
    if (data == NULL)
	return -ENOMEM;

    data->handle = nbd_api.create();
    if (data->handle == NULL) {
	note(NULL, ERROR, USER, "netfs: cannot create an NBD handle");
	TALLOC_FREE(data);
	return -ENOMEM;
    }
    mount->backend_data = data;

    /* Allow the TLS variants, and let the connection use the server's
     * block size so that writes are aligned the way it prefers.  */
    nbd_api.set_uri_allow_tls(data->handle, LIBNBD_TLS_ALLOW);
    nbd_api.set_uri_allow_local_file(data->handle, 0);
    nbd_api.set_request_block_size(data->handle, 1);
    nbd_api.set_pread_initialize(data->handle, 1);

    uri = nbd_normalize_uri(url);
    if (uri == NULL) {
	nbd_mount_close(mount);
	return -ENOMEM;
    }

    status = nbd_api.connect_uri(data->handle, uri);
    if (status < 0) {
	note(NULL, ERROR, USER, "netfs: cannot connect to %s: %s",
	     uri, nbd_api.get_error());
	free(uri);
	nbd_mount_close(mount);
	return -EIO;
    }
    free(uri);

    {
	int64_t size = nbd_api.get_size(data->handle);

	if (size < 0) {
	    note(NULL, ERROR, USER, "netfs: cannot get the size of %s: %s",
		 mount->display, nbd_api.get_error());
	    nbd_mount_close(mount);
	    return -EIO;
	}
	data->size = (uint64_t) size;
    }

    data->can_flush = nbd_api.can_flush(data->handle) > 0;

    /*
     * The filesystem driver talks to the export through the generic
     * io_manager, not through the library's unix_io_manager.
     */
    mount->custom_io = true;
    return 0;
}

static int nbd_block_size(NetfsMount *mount, uint64_t *bytes)
{
    NbdData *data = mount->backend_data;

    if (data == NULL)
	return -ENODEV;

    *bytes = data->size;
    return 0;
}

static ssize_t nbd_block_pread(NetfsMount *mount, void *buffer, size_t count,
			 uint64_t offset)
{
    NbdData *data = mount->backend_data;

    if (data == NULL)
	return -ENODEV;
    if (offset >= data->size)
	return -EINVAL;
    if (count > data->size - offset)
	count = (size_t) (data->size - offset);

    if (count == 0)
	return 0;

    if (nbd_api.pread(data->handle, buffer, count, offset, 0) < 0) {
	VERBOSE(NULL, 1, "netfs: NBD read failed: %s", nbd_api.get_error());
	return -(nbd_api.get_errno() != 0 ? nbd_api.get_errno() : EIO);
    }

    return (ssize_t) count;
}

static ssize_t nbd_block_pwrite(NetfsMount *mount, const void *buffer, size_t count,
			  uint64_t offset)
{
    NbdData *data = mount->backend_data;

    if (data == NULL)
	return -ENODEV;
    if (offset >= data->size)
	return -EINVAL;
    if (count > data->size - offset)
	count = (size_t) (data->size - offset);

    if (count == 0)
	return 0;

    /* FUA makes the write durable, which is what the filesystem driver
     * expects from a successful pwrite() before it reports fsync().  */
    if (nbd_api.pwrite(data->handle, buffer, count, offset,
		       LIBNBD_CMD_FLAG_FUA) < 0) {
	VERBOSE(NULL, 1, "netfs: NBD write failed: %s", nbd_api.get_error());
	return -(nbd_api.get_errno() != 0 ? nbd_api.get_errno() : EIO);
    }

    return (ssize_t) count;
}

static int nbd_block_flush(NetfsMount *mount)
{
    NbdData *data = mount->backend_data;

    if (data == NULL)
	return -ENODEV;

    if (data->can_flush
	&& nbd_api.flush(data->handle, 0) < 0) {
	VERBOSE(NULL, 1, "netfs: NBD flush failed: %s", nbd_api.get_error());
	return -(nbd_api.get_errno() != 0 ? nbd_api.get_errno() : EIO);
    }

    return 0;
}

const NetfsBackend netfs_backend_nbd = {
    .name = "nbd",
    .schemes = nbd_schemes,
    .kind = NETFS_KIND_BLOCK,
    .implemented = true,
    .block = {
	    .open = NULL,	/* connected by nbd_mount_init() */
	    .close = nbd_mount_close,
	    .size = nbd_block_size,
	    .pread = nbd_block_pread,
	    .pwrite = nbd_block_pwrite,
	    .flush = nbd_block_flush,
	    },
    .init = nbd_mount_init,
    .fini = nbd_mount_close,
};

#else				/* !HAVE_LIBNBD */

const NetfsBackend netfs_backend_nbd = {
    .name = "nbd",
    .schemes = nbd_schemes,
    .kind = NETFS_KIND_BLOCK,
    .implemented = false,
    .reserved_note = "this build was made without the libnbd headers",
};

#endif				/* HAVE_LIBNBD */
