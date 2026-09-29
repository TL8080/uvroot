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
 * iSCSI block backend.
 *
 * An iSCSI LUN is a byte-addressable device, so the ext2/3/4 driver
 * (fs_ext2.c) is stacked on top of it to expose a directory tree.  The
 * initiator side is handled by libiscsi, resolved with dlopen() so that
 * uvroot keeps no link-time dependency and still starts on unrooted
 * Android/Termux systems where the library is missing; when it is absent
 * the backend reports that it is not available.
 *
 * URI syntax:
 *
 *     iscsi://[<user>:<password>@]<host>[:<port>]/<target-iqn>[/<lun>]
 *     iscsi+tcp://...                       (same thing)
 *
 * The default port is 3260 and the default LUN is 0.  <user>:<password>
 * enables CHAP.  The initiator name can be changed with
 * UVROOT_NETFS_ISCSI_INITIATOR.
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

static const char *const iscsi_schemes[] = {
    "iscsi", "iscsi+tcp", NULL
};

#define ISCSI_DEFAULT_PORT "3260"
#define ISCSI_DEFAULT_INITIATOR "iqn.2026-01.net.uvroot:initiator"

#ifdef HAVE_LIBISCSI

#include <dlfcn.h>
#include <iscsi/iscsi.h>
#include <iscsi/scsi-lowlevel.h>

typedef struct IscsiApi {
    void *handle;
    struct iscsi_context *(*create_context) (const char *initiator_name);
    int (*destroy_context) (struct iscsi_context * iscsi);
    int (*set_targetname) (struct iscsi_context * iscsi,
			   const char *targetname);
    int (*set_session_type) (struct iscsi_context * iscsi,
			     enum iscsi_session_type session_type);
    int (*set_header_digest) (struct iscsi_context * iscsi,
			      enum iscsi_header_digest header_digest);
    int (*set_timeout) (struct iscsi_context * iscsi, int timeout);
    int (*set_initiator_username_pwd) (struct iscsi_context * iscsi,
				       const char *user, const char *pwd);
    int (*full_connect_sync) (struct iscsi_context * iscsi,
			      const char *portal, int lun);
    int (*disconnect) (struct iscsi_context * iscsi);
    const char *(*get_error) (struct iscsi_context * iscsi);
    struct scsi_task *(*read16_sync) (struct iscsi_context * iscsi, int lun,
				      uint64_t lba, uint32_t datalen,
				      int blocksize, int rdprotect, int dpo,
				      int fua, int fua_nv, int group_number);
    struct scsi_task *(*write16_sync) (struct iscsi_context * iscsi, int lun,
				       uint64_t lba, unsigned char *data,
				       uint32_t datalen, int blocksize,
				       int wrprotect, int dpo, int fua,
				       int fua_nv, int group_number);
    struct scsi_task *(*readcapacity16_sync) (struct iscsi_context * iscsi,
					      int lun);
    void (*free_task) (struct scsi_task *task);
} IscsiApi;

static IscsiApi iscsi_api;
static bool iscsi_api_ready = false;

typedef struct IscsiData {
    struct iscsi_context *context;
    int lun;
    uint64_t size;		/* bytes */
    uint32_t block_size;	/* server block size, usually 512 */
} IscsiData;

static void *iscsi_open_library(const char *const *candidates)
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

static bool load_iscsi_api(void)
{
    static const char *candidates[8];
    static char prefix_libiscsi[PATH_MAX];
    const char *environment;
    const char *prefix;
    void *handle;
    size_t i = 0;

    if (iscsi_api_ready)
	return true;

    environment = getenv("UVROOT_NETFS_LIBISCSI");
    if (environment != NULL && environment[0] != '\0')
	candidates[i++] = environment;

    prefix = getenv("PREFIX");
    if (prefix != NULL && prefix[0] != '\0') {
	snprintf(prefix_libiscsi, sizeof(prefix_libiscsi), "%s/lib/libiscsi.so",
		 prefix);
	candidates[i++] = prefix_libiscsi;
    }

    candidates[i++] = "libiscsi.so.11";
    candidates[i++] = "libiscsi.so.0";
    candidates[i++] = "libiscsi.so";
    candidates[i++] = NULL;

    handle = iscsi_open_library(candidates);
    if (handle == NULL) {
	VERBOSE(NULL, 1, "netfs: libiscsi not found: %s", dlerror());
	return false;
    }

    memset(&iscsi_api, 0, sizeof(iscsi_api));
    iscsi_api.handle = handle;

#define RESOLVE(field, symbol)						\
    do {								\
	*(void **) (&iscsi_api.field) = dlsym(handle, symbol);		\
	if (iscsi_api.field == NULL) {					\
	    VERBOSE(NULL, 1, "netfs: %s not found in libiscsi", symbol); \
	    dlclose(handle);						\
	    iscsi_api.handle = NULL;					\
	    return false;						\
	}								\
    } while (0)

    RESOLVE(create_context, "iscsi_create_context");
    RESOLVE(destroy_context, "iscsi_destroy_context");
    RESOLVE(set_targetname, "iscsi_set_targetname");
    RESOLVE(set_session_type, "iscsi_set_session_type");
    RESOLVE(set_header_digest, "iscsi_set_header_digest");
    RESOLVE(set_timeout, "iscsi_set_timeout");
    RESOLVE(set_initiator_username_pwd, "iscsi_set_initiator_username_pwd");
    RESOLVE(full_connect_sync, "iscsi_full_connect_sync");
    RESOLVE(disconnect, "iscsi_disconnect");
    RESOLVE(get_error, "iscsi_get_error");
    RESOLVE(read16_sync, "iscsi_read16_sync");
    RESOLVE(write16_sync, "iscsi_write16_sync");
    RESOLVE(readcapacity16_sync, "iscsi_readcapacity16_sync");
    RESOLVE(free_task, "scsi_free_scsi_task");
#undef RESOLVE

    iscsi_api_ready = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* URI parsing                                                         */
/* ------------------------------------------------------------------ */

typedef struct IscsiUri {
    char *portal;		/* host[:port] */
    char *target;		/* target IQN */
    char *user;
    char *password;
    int lun;
} IscsiUri;

static void iscsi_uri_free(IscsiUri *uri)
{
    free(uri->portal);
    free(uri->target);
    free(uri->user);
    free(uri->password);
    memset(uri, 0, sizeof(*uri));
}

/*
 * Split "iscsi://[user:pass@]host[:port]/iqn[/lun]" (or the bare
 * "host[:port]/iqn[/lun]") into its parts.
 */
static int iscsi_uri_parse(const char *url, IscsiUri *uri)
{
    char *copy;
    char *authority;
    char *path;
    char *at;
    char *slash;
    char *colon;

    memset(uri, 0, sizeof(*uri));
    uri->lun = 0;

    if (strncasecmp(url, "iscsi+tcp://", 12) == 0)
	url += 12;
    else if (strncasecmp(url, "iscsi://", 8) == 0)
	url += 8;

    copy = strdup(url);
    if (copy == NULL)
	return -ENOMEM;

    authority = copy;
    slash = strchr(authority, '/');
    if (slash == NULL) {
	note(NULL, ERROR, USER,
	     "netfs: an iSCSI target name is required "
	     "(iscsi://host/target-iqn[/lun])");
	free(copy);
	return -EINVAL;
    }
    *slash = '\0';
    path = slash + 1;

    at = strrchr(authority, '@');
    if (at != NULL) {
	*at = '\0';
	colon = strchr(authority, ':');
	if (colon != NULL) {
	    *colon = '\0';
	    uri->password = strdup(colon + 1);
	}
	uri->user = strdup(authority);
	authority = at + 1;
    }
    if (authority[0] == '\0') {
	note(NULL, ERROR, USER, "netfs: no iSCSI portal given");
	iscsi_uri_free(uri);
	free(copy);
	return -EINVAL;
    }

    /* host[:port], with "host" alone given the default port.  A bracketed
     * IPv6 literal without a port is completed too.  */
    colon = strrchr(authority, ':');
    if (colon != NULL && colon[1] != '\0'
	&& (authority[0] != '[' || strchr(authority, ']') == NULL
	    || strchr(authority, ']') < colon)) {
	uri->portal = strdup(authority);
    } else {
	size_t length = strlen(authority) + strlen(ISCSI_DEFAULT_PORT) + 2;

	if (colon != NULL && colon[1] == '\0')
	    *colon = '\0';	/* drop a trailing colon */

	uri->portal = malloc(length);
	if (uri->portal != NULL)
	    snprintf(uri->portal, length, "%s:%s", authority,
		     ISCSI_DEFAULT_PORT);
    }

    /* "/iqn.../lun": the LUN is the last component.  */
    {
	char *last = strrchr(path, '/');

	if (last != NULL && last[1] != '\0') {
	    char *end;
	    long lun = strtol(last + 1, &end, 10);

	    if (*end == '\0' && lun >= 0 && lun <= INT_MAX) {
		uri->lun = (int) lun;
		*last = '\0';
	    }
	}
    }

    if (path[0] == '\0') {
	note(NULL, ERROR, USER, "netfs: no iSCSI target name given");
	iscsi_uri_free(uri);
	free(copy);
	return -EINVAL;
    }
    uri->target = strdup(path);

    free(copy);

    if (uri->portal == NULL || uri->target == NULL
	|| (uri->user != NULL && uri->password == NULL)) {
	iscsi_uri_free(uri);
	return -ENOMEM;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Transfers                                                           */
/* ------------------------------------------------------------------ */

static int iscsi_read_blocks(IscsiData *data, uint64_t lba, uint32_t blocks,
			     void *buffer)
{
    struct scsi_task *task;
    uint32_t length = blocks * data->block_size;

    task = iscsi_api.read16_sync(data->context, data->lun, lba, length,
				 (int) data->block_size, 0, 0, 0, 0, 0);
    if (task == NULL) {
	VERBOSE(NULL, 1, "netfs: iSCSI read failed: %s",
		iscsi_api.get_error(data->context));
	return -EIO;
    }
    if (task->status != SCSI_STATUS_GOOD) {
	VERBOSE(NULL, 1, "netfs: iSCSI read status %d", task->status);
	iscsi_api.free_task(task);
	return -EIO;
    }
    if ((uint32_t) task->datain.size < length) {
	iscsi_api.free_task(task);
	return -EIO;
    }

    memcpy(buffer, task->datain.data, length);
    iscsi_api.free_task(task);
    return 0;
}

static int iscsi_write_blocks(IscsiData *data, uint64_t lba, uint32_t blocks,
			      const void *buffer)
{
    struct scsi_task *task;
    uint32_t length = blocks * data->block_size;

    task = iscsi_api.write16_sync(data->context, data->lun, lba,
				  (unsigned char *) buffer, length,
				  (int) data->block_size, 0, 0, 0, 0, 0);
    if (task == NULL) {
	VERBOSE(NULL, 1, "netfs: iSCSI write failed: %s",
		iscsi_api.get_error(data->context));
	return -EIO;
    }
    if (task->status != SCSI_STATUS_GOOD) {
	VERBOSE(NULL, 1, "netfs: iSCSI write status %d", task->status);
	iscsi_api.free_task(task);
	return -EIO;
    }

    iscsi_api.free_task(task);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Block operations                                                    */
/* ------------------------------------------------------------------ */

static ssize_t iscsi_block_pread(NetfsMount *mount, void *buffer, size_t count,
				 uint64_t offset)
{
    IscsiData *data = mount->backend_data;
    uint32_t block = data->block_size;
    uint64_t first, last;
    size_t span;
    uint8_t *scratch;
    int status;

    if (data == NULL)
	return -ENODEV;
    if (count == 0)
	return 0;
    if (offset >= data->size)
	return -EINVAL;
    if (count > data->size - offset)
	count = (size_t) (data->size - offset);

    first = offset / block;
    last = (offset + count + block - 1) / block;
    span = (size_t) ((last - first) * block);
    if (span == count) {
	status = iscsi_read_blocks(data, first, (uint32_t) (last - first),
				   buffer);
	if (status < 0)
	    return status;
	return (ssize_t) count;
    }

    scratch = talloc_size(data, span);
    if (scratch == NULL)
	return -ENOMEM;

    status = iscsi_read_blocks(data, first, (uint32_t) (last - first),
			       scratch);
    if (status < 0) {
	TALLOC_FREE(scratch);
	return status;
    }

    memcpy(buffer, scratch + (offset - first * block), count);
    TALLOC_FREE(scratch);
    return (ssize_t) count;
}

static ssize_t iscsi_block_pwrite(NetfsMount *mount, const void *buffer,
				  size_t count, uint64_t offset)
{
    IscsiData *data = mount->backend_data;
    uint32_t block = data->block_size;
    uint64_t first, last;
    size_t span;
    uint8_t *scratch;
    int status;

    if (data == NULL)
	return -ENODEV;
    if (count == 0)
	return 0;
    if (offset >= data->size)
	return -EINVAL;
    if (count > data->size - offset)
	count = (size_t) (data->size - offset);

    first = offset / block;
    last = (offset + count + block - 1) / block;
    span = (size_t) ((last - first) * block);

    if (span == count) {
	status = iscsi_write_blocks(data, first, (uint32_t) (last - first),
				    buffer);
	if (status < 0)
	    return status;
	return (ssize_t) count;
    }

    /* Unaligned write: read-modify-write the enclosing blocks.  */
    scratch = talloc_size(data, span);
    if (scratch == NULL)
	return -ENOMEM;

    status = iscsi_read_blocks(data, first, (uint32_t) (last - first),
			       scratch);
    if (status < 0) {
	TALLOC_FREE(scratch);
	return status;
    }

    memcpy(scratch + (offset - first * block), buffer, count);
    status = iscsi_write_blocks(data, first, (uint32_t) (last - first),
				scratch);
    TALLOC_FREE(scratch);
    if (status < 0)
	return status;

    return (ssize_t) count;
}

static int iscsi_block_size(NetfsMount *mount, uint64_t *bytes)
{
    IscsiData *data = mount->backend_data;

    if (data == NULL)
	return -ENODEV;

    *bytes = data->size;
    return 0;
}

static int iscsi_flush(NetfsMount *mount UNUSED)
{
    /* WRITE16 without FUA already waited for the target's completion, so
     * the data is at least in the target's hands; nothing to do here.  */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Opening and closing                                                 */
/* ------------------------------------------------------------------ */

static void iscsi_fini(NetfsMount *mount)
{
    IscsiData *data = mount->backend_data;

    if (data != NULL) {
	if (data->context != NULL) {
	    iscsi_api.disconnect(data->context);
	    iscsi_api.destroy_context(data->context);
	}
	TALLOC_FREE(data);
    }
    mount->backend_data = NULL;
}

static int iscsi_init(NetfsMount *mount, const char *url)
{
    const char *initiator;
    IscsiUri uri;
    IscsiData *data;
    struct scsi_task *task;
    int status;

    if (!load_iscsi_api())
	return -ENOSYS;

    status = iscsi_uri_parse(url, &uri);
    if (status < 0)
	return status;

    data = talloc_zero(mount, IscsiData);
    if (data == NULL) {
	iscsi_uri_free(&uri);
	return -ENOMEM;
    }
    mount->backend_data = data;

    initiator = getenv("UVROOT_NETFS_ISCSI_INITIATOR");
    if (initiator == NULL || initiator[0] == '\0')
	initiator = ISCSI_DEFAULT_INITIATOR;

    data->context = iscsi_api.create_context(initiator);
    if (data->context == NULL) {
	note(NULL, ERROR, USER, "netfs: cannot create an iSCSI context");
	iscsi_uri_free(&uri);
	iscsi_fini(mount);
	return -ENOMEM;
    }

    iscsi_api.set_timeout(data->context, 30);
    iscsi_api.set_session_type(data->context, ISCSI_SESSION_NORMAL);
    iscsi_api.set_header_digest(data->context, ISCSI_HEADER_DIGEST_NONE);

    if (iscsi_api.set_targetname(data->context, uri.target) != 0) {
	note(NULL, ERROR, USER, "netfs: invalid iSCSI target name \"%s\"",
	     uri.target);
	iscsi_uri_free(&uri);
	iscsi_fini(mount);
	return -EINVAL;
    }

    if (uri.user != NULL
	&& iscsi_api.set_initiator_username_pwd(data->context, uri.user,
						uri.password) != 0) {
	note(NULL, ERROR, USER, "netfs: cannot set the iSCSI CHAP credentials");
	iscsi_uri_free(&uri);
	iscsi_fini(mount);
	return -EINVAL;
    }

    data->lun = uri.lun;

    if (iscsi_api.full_connect_sync(data->context, uri.portal,
				    data->lun) != 0) {
	note(NULL, ERROR, USER, "netfs: cannot connect to %s (LUN %d): %s",
	     uri.portal, data->lun, iscsi_api.get_error(data->context));
	iscsi_uri_free(&uri);
	iscsi_fini(mount);
	return -EIO;
    }

    VERBOSE(NULL, 1, "netfs: iSCSI connected to %s, target %s, LUN %d",
	    uri.portal, uri.target, data->lun);
    iscsi_uri_free(&uri);

    /* READ CAPACITY (16) tells us how large the LUN is.  libiscsi hands
     * the response back in its on-the-wire big-endian form, so the
     * fields are decoded by hand (returned LBA, then block length).  */
    task = iscsi_api.readcapacity16_sync(data->context, data->lun);
    if (task == NULL || task->status != SCSI_STATUS_GOOD
	|| task->datain.size < 12) {
	note(NULL, ERROR, USER, "netfs: cannot read the capacity of the LUN");
	if (task != NULL)
	    iscsi_api.free_task(task);
	iscsi_fini(mount);
	return -EIO;
    }

    {
	const uint8_t *raw = task->datain.data;
	uint64_t last_lba = 0;
	uint32_t block_length;
	unsigned int i;

	for (i = 0; i < 8; i++)
	    last_lba = (last_lba << 8) | raw[i];
	block_length = ((uint32_t) raw[8] << 24) | ((uint32_t) raw[9] << 16)
	    | ((uint32_t) raw[10] << 8) | (uint32_t) raw[11];

	data->block_size = block_length;
	data->size = (last_lba + 1) * (uint64_t) block_length;
    }
    iscsi_api.free_task(task);

    if (data->block_size == 0) {
	note(NULL, ERROR, USER, "netfs: the LUN reports a zero block size");
	iscsi_fini(mount);
	return -EIO;
    }

    /*
     * The filesystem driver talks to the LUN through the generic
     * io_manager, not through the library's unix_io_manager.
     */
    mount->custom_io = true;

    VERBOSE(NULL, 1, "netfs: iSCSI LUN is %llu bytes, %u-byte blocks",
	    (unsigned long long) data->size, data->block_size);

    return 0;
}

const NetfsBackend netfs_backend_iscsi = {
    .name = "iscsi",
    .schemes = iscsi_schemes,
    .kind = NETFS_KIND_BLOCK,
    .implemented = true,
    .block = {
	    .open = NULL,	/* connected by iscsi_init() */
	    .close = iscsi_fini,
	    .size = iscsi_block_size,
	    .pread = iscsi_block_pread,
	    .pwrite = iscsi_block_pwrite,
	    .flush = iscsi_flush,
	    },
    .init = iscsi_init,
    .fini = iscsi_fini,
};

#else				/* !HAVE_LIBISCSI */

const NetfsBackend netfs_backend_iscsi = {
    .name = "iscsi",
    .schemes = iscsi_schemes,
    .kind = NETFS_KIND_BLOCK,
    .implemented = false,
    .reserved_note = "this build was made without the libiscsi headers",
};

#endif				/* HAVE_LIBISCSI */
