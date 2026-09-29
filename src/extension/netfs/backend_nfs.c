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
 * NFS directory backend.
 *
 * An NFS export is a directory tree, exactly like an FTP or SMB share,
 * so this backend implements NetfsDirOps directly.  The client side is
 * handled by libnfs, resolved with dlopen() so that uvroot keeps no
 * link-time dependency and still starts on unrooted Android/Termux
 * systems where the library is missing; when it is absent the backend
 * reports that it is not available.
 *
 * The URI is whatever libnfs understands:
 *
 *     nfs://server/export
 *     nfs://server/export?version=4&nfsport=2049
 *     nfs://server/export?uid=1000&gid=1000&sec=sys
 *
 * libnfs applies the query arguments (protocol version, ports, uid/gid,
 * security flavour, ...) while parsing, so they are passed through
 * untouched.  A bare "server/export" is read as nfs://.
 *
 * Like the other directory transports, NFS is refused as the guest root:
 * the protocol does not carry the host's symlink, ownership and
 * permission model faithfully enough for a whole distribution.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <talloc.h>

#include "build.h"
#include "cli/note.h"
#include "extension/netfs/netfs.h"

static const char *const nfs_schemes[] = { "nfs", "nfs3", "nfs4", NULL };

/* NFSv3 file types (NF3REG, NF3DIR, ...), as reported by readdir.  */
#define NFS_TYPE_REG	1
#define NFS_TYPE_DIR	2
#define NFS_TYPE_LNK	5

#ifdef HAVE_LIBNFS

#include <nfsc/libnfs.h>
#include <dlfcn.h>

typedef struct NfsApi {
    void *handle;
    struct nfs_context *(*init_context) (void);
    void (*destroy_context) (struct nfs_context * nfs);
    char *(*get_error) (struct nfs_context * nfs);
    void (*set_timeout) (struct nfs_context * nfs, int milliseconds);
    void (*set_autoreconnect) (struct nfs_context * nfs, int num_retries);
    void (*set_uid) (struct nfs_context * nfs, int uid);
    void (*set_gid) (struct nfs_context * nfs, int gid);
    int (*set_version) (struct nfs_context * nfs, int version);
    void (*set_nfsport) (struct nfs_context * nfs, int port);
    void (*set_mountport) (struct nfs_context * nfs, int port);
    struct nfs_url *(*parse_url_dir) (struct nfs_context * nfs,
				      const char *url);
    void (*destroy_url) (struct nfs_url *url);
    int (*mount) (struct nfs_context * nfs, const char *server,
		  const char *exportname);
    int (*umount) (struct nfs_context * nfs);
    int (*stat64) (struct nfs_context * nfs, const char *path,
		   struct nfs_stat_64 *st);
    int (*open) (struct nfs_context * nfs, const char *path, int flags,
		 struct nfsfh **nfsfh);
    int (*open2) (struct nfs_context * nfs, const char *path, int flags,
		  int mode, struct nfsfh **nfsfh);
    int (*close) (struct nfs_context * nfs, struct nfsfh *nfsfh);
    int (*pread) (struct nfs_context * nfs, struct nfsfh *nfsfh, void *buf,
		  size_t count, uint64_t offset);
    int (*pwrite) (struct nfs_context * nfs, struct nfsfh *nfsfh,
		   const void *buf, size_t count, uint64_t offset);
    int (*fsync) (struct nfs_context * nfs, struct nfsfh *nfsfh);
    int (*ftruncate) (struct nfs_context * nfs, struct nfsfh *nfsfh,
		      uint64_t length);
    int (*opendir) (struct nfs_context * nfs, const char *path,
		    struct nfsdir **nfsdir);
    struct nfsdirent *(*readdir) (struct nfs_context * nfs,
				  struct nfsdir *nfsdir);
    void (*closedir) (struct nfs_context * nfs, struct nfsdir *nfsdir);
    int (*mkdir2) (struct nfs_context * nfs, const char *path, int mode);
    int (*rmdir) (struct nfs_context * nfs, const char *path);
    int (*unlink) (struct nfs_context * nfs, const char *path);
    int (*rename) (struct nfs_context * nfs, const char *oldpath,
		   const char *newpath);
    int (*chmod) (struct nfs_context * nfs, const char *path, int mode);
    int (*chown) (struct nfs_context * nfs, const char *path, int uid,
		  int gid);
    int (*readlink) (struct nfs_context * nfs, const char *path, char *buf,
		     int bufsize);
} NfsApi;

static NfsApi nfs_api;
static bool nfs_api_ready = false;

static void *nfs_open_library(const char *const *candidates)
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

static bool load_nfs_api(void)
{
    static const char *candidates[8];
    static char prefix_libnfs[PATH_MAX];
    const char *environment;
    const char *prefix;
    void *handle;
    size_t i = 0;

    if (nfs_api_ready)
	return true;

    environment = getenv("UVROOT_NETFS_LIBNFS");
    if (environment != NULL && environment[0] != '\0')
	candidates[i++] = environment;

    prefix = getenv("PREFIX");
    if (prefix != NULL && prefix[0] != '\0') {
	snprintf(prefix_libnfs, sizeof(prefix_libnfs), "%s/lib/libnfs.so",
		 prefix);
	candidates[i++] = prefix_libnfs;
    }

    candidates[i++] = "libnfs.so.16";
    candidates[i++] = "libnfs.so";
    candidates[i++] = NULL;

    handle = nfs_open_library(candidates);
    if (handle == NULL) {
	VERBOSE(NULL, 1, "netfs: libnfs not found: %s", dlerror());
	return false;
    }

    memset(&nfs_api, 0, sizeof(nfs_api));
    nfs_api.handle = handle;

#define RESOLVE(field, symbol)						\
    do {								\
	*(void **) (&nfs_api.field) = dlsym(handle, symbol);		\
	if (nfs_api.field == NULL) {					\
	    VERBOSE(NULL, 1, "netfs: %s not found in libnfs", symbol);	\
	    dlclose(handle);						\
	    nfs_api.handle = NULL;					\
	    return false;						\
	}								\
    } while (0)

    RESOLVE(init_context, "nfs_init_context");
    RESOLVE(destroy_context, "nfs_destroy_context");
    RESOLVE(get_error, "nfs_get_error");
    RESOLVE(set_timeout, "nfs_set_timeout");
    RESOLVE(set_autoreconnect, "nfs_set_autoreconnect");
    RESOLVE(set_uid, "nfs_set_uid");
    RESOLVE(set_gid, "nfs_set_gid");
    RESOLVE(set_version, "nfs_set_version");
    RESOLVE(set_nfsport, "nfs_set_nfsport");
    RESOLVE(set_mountport, "nfs_set_mountport");
    RESOLVE(parse_url_dir, "nfs_parse_url_dir");
    RESOLVE(destroy_url, "nfs_destroy_url");
    RESOLVE(mount, "nfs_mount");
    RESOLVE(umount, "nfs_umount");
    RESOLVE(stat64, "nfs_stat64");
    RESOLVE(open, "nfs_open");
    RESOLVE(open2, "nfs_open2");
    RESOLVE(close, "nfs_close");
    RESOLVE(pread, "nfs_pread");
    RESOLVE(pwrite, "nfs_pwrite");
    RESOLVE(fsync, "nfs_fsync");
    RESOLVE(ftruncate, "nfs_ftruncate");
    RESOLVE(opendir, "nfs_opendir");
    RESOLVE(readdir, "nfs_readdir");
    RESOLVE(closedir, "nfs_closedir");
    RESOLVE(mkdir2, "nfs_mkdir2");
    RESOLVE(rmdir, "nfs_rmdir");
    RESOLVE(unlink, "nfs_unlink");
    RESOLVE(rename, "nfs_rename");
    RESOLVE(chmod, "nfs_chmod");
    RESOLVE(chown, "nfs_chown");
    RESOLVE(readlink, "nfs_readlink");
#undef RESOLVE

    nfs_api_ready = true;
    return true;
}

typedef struct NfsData {
    struct nfs_context *context;
    struct nfs_url *url;
    char *export;		/* for log messages */
} NfsData;

/*
 * Build the absolute path of @rel inside the mounted export.  libnfs
 * works with paths relative to the mount root.
 */
static void nfs_path(char *buffer, size_t size, const char *rel)
{
    if (rel == NULL || rel[0] == '\0')
	snprintf(buffer, size, "/");
    else if (rel[0] == '/')
	snprintf(buffer, size, "%s", rel);
    else
	snprintf(buffer, size, "/%s", rel);
}

static int nfs_status(NfsData *data, const char *what, const char *rel)
{
    VERBOSE(NULL, 1, "netfs: NFS %s \"%s\" failed: %s", what, rel,
	    nfs_api.get_error(data->context));
    return -EIO;
}

/* ------------------------------------------------------------------ */
/* Directory operations                                                */
/* ------------------------------------------------------------------ */

static int nfs_do_list(NetfsMount *mount, const char *rel,
		       NetfsDirent **entries, size_t *count,
		       TALLOC_CTX *context)
{
    NfsData *data = mount->backend_data;
    struct nfsdir *directory = NULL;
    struct nfsdirent *entry;
    NetfsDirent *list = NULL;
    size_t used = 0;
    size_t capacity = 0;
    char path[PATH_MAX];

    nfs_path(path, sizeof(path), rel);

    if (nfs_api.opendir(data->context, path, &directory) != 0)
	return nfs_status(data, "opendir", path);

    while ((entry = nfs_api.readdir(data->context, directory)) != NULL) {
	NetfsDirent *item;

	if (entry->name == NULL || entry->name[0] == '\0'
	    || strcmp(entry->name, ".") == 0
	    || strcmp(entry->name, "..") == 0)
	    continue;

	if (used == capacity) {
	    size_t grown = capacity == 0 ? 16 : capacity * 2;
	    NetfsDirent *resized =
		talloc_realloc(context, list, NetfsDirent, grown);

	    if (resized == NULL) {
		nfs_api.closedir(data->context, directory);
		return -ENOMEM;
	    }
	    list = resized;
	    capacity = grown;
	}

	item = &list[used];
	memset(item, 0, sizeof(*item));
	item->name = talloc_strdup(context, entry->name);
	if (item->name == NULL) {
	    nfs_api.closedir(data->context, directory);
	    return -ENOMEM;
	}

	/*
	 * The readdir attributes depend on the server: READDIRPLUS gives
	 * the full mode, a plain READDIR still gives the NFS file type
	 * (NF3REG, NF3DIR, NF3LNK, ...).  Use both, and fall back to a
	 * neutral mode when neither is available.
	 */
	item->is_dir = entry->type == NFS_TYPE_DIR
	    || S_ISDIR((mode_t) entry->mode);
	item->is_link = entry->type == NFS_TYPE_LNK
	    || S_ISLNK((mode_t) entry->mode);
	item->mode = (mode_t) entry->mode;
	if ((item->mode & S_IFMT) == 0)
	    item->mode = item->is_dir ? (S_IFDIR | 0755)
		: item->is_link ? (S_IFLNK | 0777) : (S_IFREG | 0644);
	item->size = (off_t) entry->size;
	item->mtime = (time_t) entry->mtime.tv_sec;

	if (item->is_link) {
	    char child[PATH_MAX];
	    char target[PATH_MAX];

	    if (rel != NULL && rel[0] != '\0')
		snprintf(child, sizeof(child), "/%s/%s", rel, entry->name);
	    else
		snprintf(child, sizeof(child), "/%s", entry->name);

	    /* nfs_readlink() returns 0 on success and does not
	     * terminate the buffer it fills.  */
	    if (nfs_api.readlink(data->context, child, target,
				 sizeof(target)) == 0) {
		target[sizeof(target) - 1] = '\0';
		item->target = talloc_strdup(context, target);
	    }
	}

	used++;
    }

    nfs_api.closedir(data->context, directory);

    *entries = list;
    *count = used;
    return 0;
}

static int nfs_do_stat(NetfsMount *mount, const char *rel, struct stat *st)
{
    NfsData *data = mount->backend_data;
    struct nfs_stat_64 nst;
    char path[PATH_MAX];

    nfs_path(path, sizeof(path), rel);

    memset(&nst, 0, sizeof(nst));
    if (nfs_api.stat64(data->context, path, &nst) != 0)
	return nfs_status(data, "stat", path);

    memset(st, 0, sizeof(*st));
    st->st_mode = (mode_t) nst.nfs_mode;
    st->st_uid = (uid_t) nst.nfs_uid;
    st->st_gid = (gid_t) nst.nfs_gid;
    st->st_nlink = (nlink_t) nst.nfs_nlink;
    st->st_size = (off_t) nst.nfs_size;
    st->st_mtime = (time_t) nst.nfs_mtime;
    st->st_atime = (time_t) nst.nfs_atime;
    st->st_ctime = (time_t) nst.nfs_ctime;
    return 0;
}

static int nfs_do_get(NetfsMount *mount, const char *rel, const char *local)
{
    NfsData *data = mount->backend_data;
    struct nfsfh *handle = NULL;
    uint8_t buffer[65536];
    uint64_t offset = 0;
    FILE *output;
    char path[PATH_MAX];

    nfs_path(path, sizeof(path), rel);

    if (nfs_api.open(data->context, path, O_RDONLY, &handle) != 0)
	return nfs_status(data, "open", path);

    output = fopen(local, "wb");
    if (output == NULL) {
	int saved = errno;

	nfs_api.close(data->context, handle);
	return -saved;
    }

    for (;;) {
	int got = nfs_api.pread(data->context, handle, buffer,
				sizeof(buffer), offset);

	if (got < 0) {
	    fclose(output);
	    nfs_api.close(data->context, handle);
	    return nfs_status(data, "read", path);
	}
	if (got == 0)
	    break;
	if (fwrite(buffer, 1, (size_t) got, output) != (size_t) got) {
	    int saved = errno;

	    fclose(output);
	    nfs_api.close(data->context, handle);
	    return -saved;
	}
	offset += (uint64_t) got;
    }

    fclose(output);
    nfs_api.close(data->context, handle);
    return 0;
}

static int nfs_do_put(NetfsMount *mount, const char *rel, const char *local)
{
    NfsData *data = mount->backend_data;
    struct nfsfh *handle = NULL;
    struct stat source;
    uint8_t buffer[65536];
    uint64_t offset = 0;
    uint64_t total = 0;
    FILE *input;
    mode_t mode = 0644;
    char path[PATH_MAX];

    nfs_path(path, sizeof(path), rel);

    if (stat(local, &source) == 0)
	mode = source.st_mode & 07777;

    if (nfs_api.open2(data->context, path,
		      O_WRONLY | O_CREAT | O_TRUNC, (int) mode,
		      &handle) != 0)
	return nfs_status(data, "create", path);

    input = fopen(local, "rb");
    if (input == NULL) {
	int saved = errno;

	nfs_api.close(data->context, handle);
	return -saved;
    }

    for (;;) {
	size_t got = fread(buffer, 1, sizeof(buffer), input);
	uint64_t written = 0;

	if (got == 0)
	    break;

	while (written < got) {
	    int put = nfs_api.pwrite(data->context, handle,
				     buffer + written,
				     (size_t) (got - written),
				     offset + written);

	    if (put < 0) {
		fclose(input);
		nfs_api.close(data->context, handle);
		return nfs_status(data, "write", path);
	    }
	    written += (uint64_t) put;
	}

	offset += got;
	total += got;
    }
    fclose(input);

    if (nfs_api.ftruncate(data->context, handle, total) != 0) {
	nfs_api.close(data->context, handle);
	return nfs_status(data, "truncate", path);
    }

    (void) nfs_api.fsync(data->context, handle);
    nfs_api.close(data->context, handle);
    return 0;
}

static int nfs_do_mkdir(NetfsMount *mount, const char *rel)
{
    NfsData *data = mount->backend_data;
    char path[PATH_MAX];

    nfs_path(path, sizeof(path), rel);
    if (nfs_api.mkdir2(data->context, path, 0755) != 0)
	return nfs_status(data, "mkdir", path);
    return 0;
}

static int nfs_do_rmdir(NetfsMount *mount, const char *rel)
{
    NfsData *data = mount->backend_data;
    char path[PATH_MAX];

    nfs_path(path, sizeof(path), rel);
    if (nfs_api.rmdir(data->context, path) != 0)
	return nfs_status(data, "rmdir", path);
    return 0;
}

static int nfs_do_unlink(NetfsMount *mount, const char *rel)
{
    NfsData *data = mount->backend_data;
    char path[PATH_MAX];

    nfs_path(path, sizeof(path), rel);
    if (nfs_api.unlink(data->context, path) != 0)
	return nfs_status(data, "unlink", path);
    return 0;
}

static int nfs_do_rename(NetfsMount *mount, const char *from, const char *to)
{
    NfsData *data = mount->backend_data;
    char old_path[PATH_MAX];
    char new_path[PATH_MAX];

    nfs_path(old_path, sizeof(old_path), from);
    nfs_path(new_path, sizeof(new_path), to);
    if (nfs_api.rename(data->context, old_path, new_path) != 0)
	return nfs_status(data, "rename", old_path);
    return 0;
}

static int nfs_do_chmod(NetfsMount *mount, const char *rel, mode_t mode)
{
    NfsData *data = mount->backend_data;
    char path[PATH_MAX];

    nfs_path(path, sizeof(path), rel);
    if (nfs_api.chmod(data->context, path, (int) (mode & 07777)) != 0)
	return nfs_status(data, "chmod", path);
    return 0;
}

static int nfs_do_chown(NetfsMount *mount, const char *rel, uid_t uid,
			gid_t gid)
{
    NfsData *data = mount->backend_data;
    char path[PATH_MAX];

    nfs_path(path, sizeof(path), rel);
    if (nfs_api.chown(data->context, path, (int) uid, (int) gid) != 0)
	return nfs_status(data, "chown", path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Opening and closing                                                 */
/* ------------------------------------------------------------------ */

static void nfs_fini(NetfsMount *mount)
{
    NfsData *data = mount->backend_data;

    if (data != NULL) {
	if (data->context != NULL) {
	    nfs_api.umount(data->context);
	    nfs_api.destroy_context(data->context);
	}
	if (data->url != NULL)
	    nfs_api.destroy_url(data->url);
	TALLOC_FREE(data);
    }
    mount->backend_data = NULL;
}

/*
 * libnfs expects an nfs:// URI; a bare "server/export" is completed so
 * that --nfs=/mnt/n:server/export works as well.
 */
static char *nfs_normalize_uri(const char *url)
{
    if (strstr(url, "://") != NULL)
	return strdup(url);

    {
	size_t length = strlen("nfs://") + strlen(url) + 1;
	char *result = malloc(length);

	if (result != NULL)
	    snprintf(result, length, "nfs://%s", url);
	return result;
    }
}

static int nfs_init(NetfsMount *mount, const char *url)
{
    NfsData *data;
    char *uri;
    int status;

    if (!load_nfs_api())
	return -ENOSYS;

    data = talloc_zero(mount, NfsData);
    if (data == NULL)
	return -ENOMEM;
    mount->backend_data = data;

    data->context = nfs_api.init_context();
    if (data->context == NULL) {
	note(NULL, ERROR, USER, "netfs: cannot create an NFS context");
	nfs_fini(mount);
	return -ENOMEM;
    }
    nfs_api.set_timeout(data->context, 30000);
    nfs_api.set_autoreconnect(data->context, 3);
    nfs_api.set_uid(data->context, (int) getuid());
    nfs_api.set_gid(data->context, (int) getgid());

    uri = nfs_normalize_uri(url);
    if (uri == NULL) {
	nfs_fini(mount);
	return -ENOMEM;
    }

    data->url = nfs_api.parse_url_dir(data->context, uri);
    if (data->url == NULL || data->url->server == NULL
	|| data->url->path == NULL) {
	note(NULL, ERROR, USER, "netfs: invalid NFS URI \"%s\": %s",
	     uri, nfs_api.get_error(data->context));
	free(uri);
	nfs_fini(mount);
	return -EINVAL;
    }

    data->export = talloc_strdup(data, data->url->path);
    if (data->export == NULL) {
	free(uri);
	nfs_fini(mount);
	return -ENOMEM;
    }

    status = nfs_api.mount(data->context, data->url->server, data->url->path);
    if (status != 0) {
	note(NULL, ERROR, USER, "netfs: cannot mount %s:%s: %s",
	     data->url->server, data->url->path,
	     nfs_api.get_error(data->context));
	free(uri);
	nfs_fini(mount);
	return -EIO;
    }
    free(uri);

    VERBOSE(NULL, 1, "netfs: NFS mounted %s:%s",
	    data->url->server, data->url->path);
    return 0;
}

const NetfsBackend netfs_backend_nfs = {
    .name = "nfs",
    .schemes = nfs_schemes,
    .kind = NETFS_KIND_DIR,
    .implemented = true,
    .dir = {
	    .list = nfs_do_list,
	    .stat = nfs_do_stat,
	    .get = nfs_do_get,
	    .put = nfs_do_put,
	    .mkdir = nfs_do_mkdir,
	    .rmdir = nfs_do_rmdir,
	    .unlink = nfs_do_unlink,
	    .rename = nfs_do_rename,
	    .chmod = nfs_do_chmod,
	    .chown = nfs_do_chown,
	    },
    .init = nfs_init,
    .fini = nfs_fini,
};

#else				/* !HAVE_LIBNFS */

const NetfsBackend netfs_backend_nfs = {
    .name = "nfs",
    .schemes = nfs_schemes,
    .kind = NETFS_KIND_DIR,
    .implemented = false,
    .reserved_note = "this build was made without the libnfs headers",
};

#endif				/* HAVE_LIBNFS */
