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
 * SMB / CIFS backend, built on libsmbclient (SMB2/SMB3 capable).
 *
 * As with the FTP backend the library is resolved with dlopen() so
 * that uvroot keeps no hard dependency and still runs on unrooted
 * Android where libsmbclient is usually absent.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include "build.h"
#include "cli/note.h"
#include "extension/netfs/netfs.h"

static const char *const smb_schemes[] = { "smb", "cifs", NULL };

#ifdef HAVE_LIBSMBCLIENT

#include <libsmbclient.h>
#include <dlfcn.h>

typedef struct SmbApi {
    void *handle;
    SMBCCTX *(*new_context) (void);
    SMBCCTX *(*init_context) (SMBCCTX * context);
    void (*free_context) (SMBCCTX * context, int shutdown_ctx);
    SMBCCTX *(*set_context) (SMBCCTX * context);
    void (*set_auth) (SMBCCTX * context, smbc_get_auth_data_fn fn);
    void (*set_debug) (SMBCCTX * context, int debug);
    int (*opendir) (const char *durl);
    struct smbc_dirent *(*readdir) (unsigned int dh);
    int (*closedir) (int dh);
    int (*stat) (const char *url, struct stat *st);
    int (*open) (const char *furl, int flags, mode_t mode);
    ssize_t(*read) (int fd, void *buf, size_t bufsize);
    ssize_t(*write) (int fd, const void *buf, size_t bufsize);
    int (*close) (int fd);
    int (*unlink) (const char *furl);
    int (*mkdir) (const char *durl, mode_t mode);
    int (*rmdir) (const char *durl);
    int (*rename) (const char *ourl, const char *nurl);
} SmbApi;

static SmbApi smb_api;
static bool smb_api_ready = false;
static bool smb_initialized = false;
static SMBCCTX *smb_context = NULL;

typedef struct SmbMountData {
    char *base;
} SmbMountData;

/*
 * Credentials gathered from the mount URLs.  libsmbclient refuses to
 * initialize when no authentication callback is installed, and the
 * callback has no user data pointer, so the credentials are kept in
 * this small global list and looked up by server name.
 */
typedef struct SmbCredential {
    struct SmbCredential *next;
    char server[256];
    char workgroup[128];
    char username[256];
    char password[256];
} SmbCredential;

static SmbCredential *smb_credentials = NULL;

static void smb_auth_data(const char *server, const char *share UNUSED,
			  char *workgroup, int wgmaxlen, char *username,
			  int unamaxlen, char *password, int pwmaxlen)
{
    SmbCredential *credential;
    SmbCredential *fallback = NULL;

    for (credential = smb_credentials; credential != NULL;
	 credential = credential->next) {
	fallback = credential;
	if (server != NULL && strcasecmp(credential->server, server) == 0)
	    break;
    }

    if (credential == NULL)
	credential = fallback;
    if (credential == NULL)
	return;

    snprintf(workgroup, wgmaxlen, "%s", credential->workgroup);
    snprintf(username, unamaxlen, "%s", credential->username);
    snprintf(password, pwmaxlen, "%s", credential->password);
}

static void copy_field(char *destination, size_t size, const char *source)
{
    size_t length;

    if (size == 0)
	return;

    length = strlen(source);
    if (length >= size)
	length = size - 1;

    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void register_smb_credentials(const char *url)
{
    SmbCredential *credential;
    const char *cursor;
    char authority[1024];
    char *at;
    char *host;
    char *credentials;
    size_t length;

    cursor = strstr(url, "://");
    cursor = cursor != NULL ? cursor + 3 : url;

    {
	const char *slash = strchr(cursor, '/');

	length = slash != NULL ? (size_t) (slash - cursor) : strlen(cursor);
	if (length >= sizeof(authority))
	    length = sizeof(authority) - 1;
	memcpy(authority, cursor, length);
	authority[length] = '\0';
    }

    credential = calloc(1, sizeof(*credential));
    if (credential == NULL)
	return;

    at = strrchr(authority, '@');
    if (at != NULL) {
	*at = '\0';
	credentials = authority;
	host = at + 1;
    } else {
	credentials = NULL;
	host = authority;
    }

    if (credentials != NULL) {
	char *semicolon = strchr(credentials, ';');
	char *colon;

	if (semicolon != NULL) {
	    *semicolon = '\0';
	    copy_field(credential->workgroup, sizeof(credential->workgroup),
		       credentials);
	    credentials = semicolon + 1;
	}

	colon = strchr(credentials, ':');
	if (colon != NULL) {
	    *colon = '\0';
	    copy_field(credential->password, sizeof(credential->password),
		       colon + 1);
	}
	copy_field(credential->username, sizeof(credential->username),
		   credentials);
    }

    /* Drop a ":port" suffix, but keep bracketed IPv6 literals intact.  */
    if (host[0] != '[') {
	char *colon = strrchr(host, ':');

	if (colon != NULL && colon[1] != '\0'
	    && strspn(colon + 1, "0123456789") == strlen(colon + 1))
	    *colon = '\0';
    }

    copy_field(credential->server, sizeof(credential->server), host);
    credential->next = smb_credentials;
    smb_credentials = credential;
}

/*
 * Release the Samba context so that uvroot's talloc leak report at exit
 * stays clean.  Registered with atexit() after uvroot's own handlers,
 * which therefore run later (atexit is LIFO).
 *
 * A few Samba globals (the module registry and the loadparm storage)
 * survive smbc_free_context(); they are harmless but would otherwise
 * be dumped by the leak report on every SMB invocation, so null
 * tracking is switched off once the context is gone.  This happens at
 * the very end of the process, after every uvroot cleanup handler.
 */
static void smb_cleanup(void)
{
    if (smb_context != NULL && smb_api.free_context != NULL) {
	smb_api.free_context(smb_context, 1);
	smb_context = NULL;
	talloc_disable_null_tracking();
    }
}

static void *smb_dlopen_first(const char *const *candidates)
{
    size_t i;

    for (i = 0; candidates[i] != NULL; i++) {
	if (candidates[i][0] == '\0')
	    continue;
	{
	    void *handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
	    if (handle != NULL)
		return handle;
	}
    }

    return NULL;
}

static bool load_smb_api(Tracee *tracee)
{
    static const char *candidates[8];
    static char prefix_libsmb[PATH_MAX];
    const char *environment;
    const char *prefix;
    void *handle;
    size_t i = 0;

    if (smb_api_ready)
	return true;

    environment = getenv("UVROOT_NETFS_LIBSMBCLIENT");
    if (environment != NULL && environment[0] != '\0')
	candidates[i++] = environment;

    prefix = getenv("PREFIX");
    if (prefix != NULL && prefix[0] != '\0') {
	snprintf(prefix_libsmb, sizeof(prefix_libsmb),
		 "%s/lib/libsmbclient.so", prefix);
	candidates[i++] = prefix_libsmb;
    }

    candidates[i++] = "libsmbclient.so.0";
    candidates[i++] = "libsmbclient.so";
    candidates[i++] = NULL;

    handle = smb_dlopen_first(candidates);
    if (handle == NULL) {
	VERBOSE(tracee, 1, "netfs: libsmbclient not found: %s", dlerror());
	return false;
    }

    memset(&smb_api, 0, sizeof(smb_api));
    smb_api.handle = handle;

#define RESOLVE(field, symbol)						\
    do {								\
	*(void **) (&smb_api.field) = dlsym(handle, symbol);		\
	if (smb_api.field == NULL) {					\
	    VERBOSE(tracee, 1, "netfs: %s not found in libsmbclient", symbol); \
	    dlclose(handle);						\
	    smb_api.handle = NULL;					\
	    return false;						\
	}								\
    } while (0)

    RESOLVE(new_context, "smbc_new_context");
    RESOLVE(init_context, "smbc_init_context");
    RESOLVE(free_context, "smbc_free_context");
    RESOLVE(set_context, "smbc_set_context");
    RESOLVE(set_auth, "smbc_setFunctionAuthData");
    RESOLVE(set_debug, "smbc_setDebug");
    RESOLVE(opendir, "smbc_opendir");
    RESOLVE(readdir, "smbc_readdir");
    RESOLVE(closedir, "smbc_closedir");
    RESOLVE(stat, "smbc_stat");
    RESOLVE(open, "smbc_open");
    RESOLVE(read, "smbc_read");
    RESOLVE(write, "smbc_write");
    RESOLVE(close, "smbc_close");
    RESOLVE(unlink, "smbc_unlink");
    RESOLVE(mkdir, "smbc_mkdir");
    RESOLVE(rmdir, "smbc_rmdir");
    RESOLVE(rename, "smbc_rename");

#undef RESOLVE

    if (!smb_initialized) {
	SMBCCTX *context = smb_api.new_context();

	if (context == NULL) {
	    VERBOSE(tracee, 1, "netfs: smbc_new_context failed");
	    dlclose(handle);
	    smb_api.handle = NULL;
	    return false;
	}

	smb_api.set_debug(context, 0);
	smb_api.set_auth(context, smb_auth_data);

	if (smb_api.init_context(context) == NULL) {
	    VERBOSE(tracee, 1, "netfs: smbc_init_context failed");
	    smb_api.free_context(context, 0);
	    dlclose(handle);
	    smb_api.handle = NULL;
	    return false;
	}

	smb_context = context;
	smb_api.set_context(smb_context);
	smb_initialized = true;

	/* Samba keeps a top level talloc context alive; free it before
	 * uvroot's own talloc leak report runs.  */
	atexit(smb_cleanup);
    }

    smb_api_ready = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* URL helpers                                                         */
/* ------------------------------------------------------------------ */

static void smb_escape(const char *input, char *output, size_t size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    const unsigned char *cursor;

    for (cursor = (const unsigned char *)input; *cursor != '\0'; cursor++) {
	unsigned char c = *cursor;
	bool unreserved = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
			   || (c >= '0' && c <= '9') || c == '-' || c == '_'
			   || c == '.' || c == '~');

	if (unreserved) {
	    if (used + 1 >= size)
		break;
	    output[used++] = (char) c;
	} else {
	    if (used + 3 >= size)
		break;
	    output[used++] = '%';
	    output[used++] = hex[c >> 4];
	    output[used++] = hex[c & 0x0f];
	}
    }

    if (size > 0)
	output[used < size ? used : size - 1] = '\0';
}

static void smb_build_url(NetfsMount *mount, const char *rel,
			  const char *extra, char *output, size_t size)
{
    SmbMountData *data = mount->backend_data;
    const char *cursor = rel;
    size_t used;

    snprintf(output, size, "%s", data->base);
    used = strlen(output);

    while (*cursor != '\0' && used + 1 < size) {
	const char *slash = strchr(cursor, '/');
	size_t length = slash != NULL ? (size_t) (slash - cursor) : strlen(cursor);
	char name[NAME_MAX + 1];
	char escaped[NAME_MAX * 3 + 1];

	if (length == 0) {
	    cursor++;
	    continue;
	}
	if (length > NAME_MAX)
	    break;

	memcpy(name, cursor, length);
	name[length] = '\0';
	smb_escape(name, escaped, sizeof(escaped));

	output[used++] = '/';
	snprintf(output + used, size - used, "%s", escaped);
	used = strlen(output);

	cursor = slash != NULL ? slash + 1 : cursor + length;
    }

    if (extra != NULL && extra[0] != '\0' && used + 1 < size) {
	char escaped[NAME_MAX * 3 + 1];

	smb_escape(extra, escaped, sizeof(escaped));
	output[used++] = '/';
	snprintf(output + used, size - used, "%s", escaped);
    }
}

/* ------------------------------------------------------------------ */
/* Directory operations                                                */
/* ------------------------------------------------------------------ */

static int smb_list(NetfsMount *mount, const char *rel, NetfsDirent **entries,
		    size_t *count, TALLOC_CTX *context)
{
    NetfsDirent *array = NULL;
    size_t used = 0;
    size_t capacity = 0;
    char url[PATH_MAX * 3];
    int directory;
    struct smbc_dirent *item;

    if (rel[0] == '\0')
	snprintf(url, sizeof(url), "%s", ((SmbMountData *) mount->backend_data)->base);
    else
	smb_build_url(mount, rel, NULL, url, sizeof(url));

    directory = smb_api.opendir(url);
    if (directory < 0)
	return -errno;

    while ((item = smb_api.readdir(directory)) != NULL) {
	NetfsDirent *entry;
	bool is_dir;

	if (item->name[0] == '\0')
	    continue;
	if (strcmp(item->name, ".") == 0 || strcmp(item->name, "..") == 0)
	    continue;

	is_dir = (item->smbc_type == SMBC_DIR
		  || item->smbc_type == SMBC_FILE_SHARE);

	if (used == capacity) {
	    size_t grown_capacity = capacity == 0 ? 16 : capacity * 2;
	    NetfsDirent *grown;

	    grown = talloc_realloc(context, array, NetfsDirent,
				   grown_capacity);
	    if (grown == NULL) {
		smb_api.closedir(directory);
		return -ENOMEM;
	    }
	    array = grown;
	    capacity = grown_capacity;
	}

	entry = &array[used];
	memset(entry, 0, sizeof(*entry));
	entry->name = talloc_strdup(context, item->name);
	if (entry->name == NULL) {
	    smb_api.closedir(directory);
	    return -ENOMEM;
	}
	entry->is_dir = is_dir;
	entry->mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);

	{
	    char child[PATH_MAX * 3];
	    struct stat st;

	    smb_build_url(mount, rel, item->name, child, sizeof(child));
	    if (smb_api.stat(child, &st) == 0) {
		entry->size = st.st_size;
		entry->mtime = st.st_mtime;
		entry->mode = st.st_mode;
		entry->is_dir = S_ISDIR(st.st_mode);
	    }
	}

	used++;
    }

    smb_api.closedir(directory);

    *entries = array;
    *count = used;
    return 0;
}

static int smb_stat(NetfsMount *mount, const char *rel, struct stat *st)
{
    char url[PATH_MAX * 3];

    if (rel[0] == '\0') {
	smb_build_url(mount, "", NULL, url, sizeof(url));
    } else {
	smb_build_url(mount, rel, NULL, url, sizeof(url));
    }

    if (smb_api.stat(url, st) < 0)
	return -errno;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Content transfer                                                    */
/* ------------------------------------------------------------------ */

static int smb_get(NetfsMount *mount, const char *rel, const char *local)
{
    char url[PATH_MAX * 3];
    char buffer[65536];
    FILE *file;
    int fd;
    ssize_t got;

    smb_build_url(mount, rel, NULL, url, sizeof(url));

    fd = smb_api.open(url, O_RDONLY, 0);
    if (fd < 0)
	return -errno;

    file = fopen(local, "wb");
    if (file == NULL) {
	int saved = errno;
	smb_api.close(fd);
	return -saved;
    }

    while ((got = smb_api.read(fd, buffer, sizeof(buffer))) > 0) {
	if (fwrite(buffer, 1, (size_t) got, file) != (size_t) got) {
	    int saved = errno;
	    fclose(file);
	    smb_api.close(fd);
	    return -saved;
	}
    }

    smb_api.close(fd);
    if (fclose(file) != 0)
	return -EIO;
    if (got < 0)
	return -errno;

    return 0;
}

static int smb_put(NetfsMount *mount, const char *rel, const char *local)
{
    char url[PATH_MAX * 3];
    char buffer[65536];
    FILE *file;
    int fd;
    size_t got;

    smb_build_url(mount, rel, NULL, url, sizeof(url));

    fd = smb_api.open(url, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
	return -errno;

    file = fopen(local, "rb");
    if (file == NULL) {
	int saved = errno;
	smb_api.close(fd);
	return -saved;
    }

    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
	ssize_t written = smb_api.write(fd, buffer, got);

	if (written != (ssize_t) got) {
	    int saved = errno != 0 ? errno : EIO;
	    fclose(file);
	    smb_api.close(fd);
	    return -saved;
	}
    }

    fclose(file);
    smb_api.close(fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Namespace operations                                                */
/* ------------------------------------------------------------------ */

static int smb_mkdir(NetfsMount *mount, const char *rel)
{
    char url[PATH_MAX * 3];

    smb_build_url(mount, rel, NULL, url, sizeof(url));
    if (smb_api.mkdir(url, 0755) < 0)
	return -errno;
    return 0;
}

static int smb_rmdir(NetfsMount *mount, const char *rel)
{
    char url[PATH_MAX * 3];

    smb_build_url(mount, rel, NULL, url, sizeof(url));
    if (smb_api.rmdir(url) < 0)
	return -errno;
    return 0;
}

static int smb_unlink(NetfsMount *mount, const char *rel)
{
    char url[PATH_MAX * 3];

    smb_build_url(mount, rel, NULL, url, sizeof(url));
    if (smb_api.unlink(url) < 0)
	return -errno;
    return 0;
}

static int smb_rename(NetfsMount *mount, const char *from, const char *to)
{
    char old_url[PATH_MAX * 3];
    char new_url[PATH_MAX * 3];

    smb_build_url(mount, from, NULL, old_url, sizeof(old_url));
    smb_build_url(mount, to, NULL, new_url, sizeof(new_url));

    if (smb_api.rename(old_url, new_url) < 0)
	return -errno;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Backend plumbing                                                    */
/* ------------------------------------------------------------------ */

static void smb_fini(NetfsMount *mount)
{
    if (mount->backend_data != NULL)
	TALLOC_FREE(mount->backend_data);
    mount->backend_data = NULL;
}

static int smb_init(NetfsMount *mount, const char *url)
{
    SmbMountData *data;
    char *base;
    size_t length;

    /* Register credentials before libsmbclient is initialized, since
     * the authentication callback is installed at that point.  */
    register_smb_credentials(url);

    if (!load_smb_api(NULL)) {
	note(NULL, WARNING, USER,
	     "netfs: this build has libsmbclient support but libsmbclient "
	     "could not be loaded at run time (set UVROOT_NETFS_LIBSMBCLIENT "
	     "to its path)");
	return -ENOSYS;
    }

    data = talloc_zero(mount, SmbMountData);
    if (data == NULL)
	return -ENOMEM;

    base = talloc_strdup(data, url);
    if (base == NULL)
	return -ENOMEM;

    length = strlen(base);
    while (length > 0 && base[length - 1] == '/' && length > strlen("smb://"))
	base[--length] = '\0';

    data->base = base;
    mount->backend_data = data;
    return 0;
}

const NetfsBackend netfs_backend_smb = {
    .name = "smb",
    .schemes = smb_schemes,
    .kind = NETFS_KIND_DIR,
    .implemented = true,
    .dir = {
	    .list = smb_list,
	    .stat = smb_stat,
	    .get = smb_get,
	    .put = smb_put,
	    .mkdir = smb_mkdir,
	    .rmdir = smb_rmdir,
	    .unlink = smb_unlink,
	    .rename = smb_rename,
	    },
    .init = smb_init,
    .fini = smb_fini,
};

#else				/* !HAVE_LIBSMBCLIENT */

const NetfsBackend netfs_backend_smb = {
    .name = "smb",
    .schemes = smb_schemes,
    .kind = NETFS_KIND_DIR,
    .implemented = false,
    .reserved_note = "this build was made without libsmbclient headers",
};

#endif				/* HAVE_LIBSMBCLIENT */
