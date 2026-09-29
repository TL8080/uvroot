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
 * FTP / FTPS / FTPES / SFTP backend.
 *
 * libcurl is resolved at run time with dlopen() so that uvroot itself
 * keeps no library dependency and keeps working on unrooted Android /
 * Termux systems where libcurl may or may not be installed.  When the
 * library is missing the backend simply reports that it is not
 * available.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "build.h"
#include "cli/note.h"
#include "extension/netfs/netfs.h"

static const char *const ftp_schemes[] = { "ftp", "ftps", "ftpes", "sftp", NULL };

#ifdef HAVE_LIBCURL

#include <curl/curl.h>
#include <dlfcn.h>

typedef struct CurlApi {
    void *handle;
    CURLcode(*global_init) (long flags);
    void (*global_cleanup) (void);
    CURL *(*easy_init) (void);
    void (*easy_cleanup) (CURL * handle);
    CURLcode(*easy_setopt) (CURL * handle, CURLoption option, ...);
    CURLcode(*easy_perform) (CURL * handle);
    const char *(*easy_strerror) (CURLcode code);
    struct curl_slist *(*slist_append) (struct curl_slist * list,
					const char *data);
    void (*slist_free_all) (struct curl_slist * list);
    time_t(*getdate) (const char *string, const time_t * unused);
} CurlApi;

static CurlApi curl_api;
static bool curl_api_ready = false;

typedef struct CurlMountData {
    char *base;			/* URL without trailing slash */
    char *remote_path;		/* absolute path part of the URL */
    long use_ssl;
    bool sftp;
} CurlMountData;

static void *open_first(const char *const *candidates)
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

static bool load_curl_api(Tracee *tracee)
{
    static const char *candidates[8];
    static char prefix_libcurl[PATH_MAX];
    const char *environment;
    const char *prefix;
    void *handle;
    size_t i = 0;

    if (curl_api_ready)
	return true;

    environment = getenv("UVROOT_NETFS_LIBCURL");
    if (environment != NULL && environment[0] != '\0')
	candidates[i++] = environment;

    prefix = getenv("PREFIX");
    if (prefix != NULL && prefix[0] != '\0') {
	snprintf(prefix_libcurl, sizeof(prefix_libcurl), "%s/lib/libcurl.so",
		 prefix);
	candidates[i++] = prefix_libcurl;
    }

    candidates[i++] = "libcurl.so.4";
    candidates[i++] = "libcurl.so";
    candidates[i++] = NULL;

    handle = open_first(candidates);
    if (handle == NULL) {
	VERBOSE(tracee, 1, "netfs: libcurl not found: %s", dlerror());
	return false;
    }

    memset(&curl_api, 0, sizeof(curl_api));
    curl_api.handle = handle;

#define RESOLVE(field, symbol)						\
    do {								\
	*(void **) (&curl_api.field) = dlsym(handle, symbol);		\
	if (curl_api.field == NULL) {					\
	    VERBOSE(tracee, 1, "netfs: %s not found in libcurl", symbol); \
	    dlclose(handle);						\
	    curl_api.handle = NULL;					\
	    return false;						\
	}								\
    } while (0)

    RESOLVE(global_init, "curl_global_init");
    RESOLVE(global_cleanup, "curl_global_cleanup");
    RESOLVE(easy_init, "curl_easy_init");
    RESOLVE(easy_cleanup, "curl_easy_cleanup");
    RESOLVE(easy_setopt, "curl_easy_setopt");
    RESOLVE(easy_perform, "curl_easy_perform");
    RESOLVE(easy_strerror, "curl_easy_strerror");
    RESOLVE(slist_append, "curl_slist_append");
    RESOLVE(slist_free_all, "curl_slist_free_all");

#undef RESOLVE

    /* Optional.  */
    *(void **) (&curl_api.getdate) = dlsym(handle, "curl_getdate");

    curl_api.global_init(CURL_GLOBAL_DEFAULT);
    curl_api_ready = true;
    return true;
}

static int curl_code_to_errno(CURLcode code)
{
    switch (code) {
    case CURLE_OK:
	return 0;
    case CURLE_REMOTE_FILE_NOT_FOUND:
    case CURLE_FTP_COULDNT_RETR_FILE:
    case CURLE_TFTP_NOTFOUND:
	return -ENOENT;
    case CURLE_LOGIN_DENIED:
    case CURLE_REMOTE_ACCESS_DENIED:
	return -EACCES;
    case CURLE_COULDNT_RESOLVE_HOST:
	return -EHOSTUNREACH;
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
	return -ETIMEDOUT;
    case CURLE_QUOTE_ERROR:
	return -EIO;
    case CURLE_OUT_OF_MEMORY:
	return -ENOMEM;
    case CURLE_UNSUPPORTED_PROTOCOL:
	return -EPROTONOSUPPORT;
    default:
	return -EIO;
    }
}

static size_t discard_write(void *ptr UNUSED, size_t size, size_t nmemb,
			    void *userdata UNUSED)
{
    return size * nmemb;
}

/*
 * Report the libcurl level error (visible with -v) and translate the
 * code into an errno.
 */
static int report_curl_error(const char *operation, const char *url,
			     CURLcode code)
{
    if (code == CURLE_OK)
	return 0;

    VERBOSE(NULL, 1, "netfs: %s \"%s\" failed: %s", operation, url,
	    curl_api.easy_strerror(code));

    return curl_code_to_errno(code);
}

/* ------------------------------------------------------------------ */
/* URL helpers                                                         */
/* ------------------------------------------------------------------ */

static void url_escape(const char *input, char *output, size_t size)
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

/* Build the URL of @rel.  When @directory is true a trailing slash is
 * appended, as expected by FTP directory listings.  */
static void build_url(NetfsMount *mount, const char *rel, bool directory,
		      char *output, size_t size)
{
    CurlMountData *data = mount->backend_data;
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
	url_escape(name, escaped, sizeof(escaped));

	output[used++] = '/';
	snprintf(output + used, size - used, "%s", escaped);
	used = strlen(output);

	cursor = slash != NULL ? slash + 1 : cursor + length;
    }

    if (directory && used + 1 < size) {
	output[used++] = '/';
	output[used] = '\0';
    }
}

/* Parent directory URL, used to run FTP/SFTP quote commands on a
 * transfer that is expected to succeed.  */
static void build_parent_url(NetfsMount *mount, const char *rel,
			     char *output, size_t size)
{
    const char *slash = strrchr(rel, '/');

    if (slash == NULL)
	build_url(mount, "", true, output, size);
    else {
	char parent[PATH_MAX];

	if ((size_t) (slash - rel) >= sizeof(parent))
	    snprintf(parent, sizeof(parent), "%s", rel);
	else {
	    memcpy(parent, rel, slash - rel);
	    parent[slash - rel] = '\0';
	}
	build_url(mount, parent, true, output, size);
    }
}

static void remote_abspath(NetfsMount *mount, const char *rel,
			   char *output, size_t size)
{
    CurlMountData *data = mount->backend_data;

    if (rel[0] == '\0')
	snprintf(output, size, "%s", data->remote_path);
    else if (strcmp(data->remote_path, "/") == 0)
	snprintf(output, size, "/%s", rel);
    else
	snprintf(output, size, "%s/%s", data->remote_path, rel);
}

/*
 * Accept whatever host key the SFTP server presents.  Only installed
 * when UVROOT_NETFS_INSECURE is set; the default remains libcurl's
 * known_hosts verification.
 */
static int ssh_key_accept(CURL *handle UNUSED,
			  const struct curl_khkey *known UNUSED,
			  const struct curl_khkey *found UNUSED,
			  enum curl_khmatch match UNUSED,
			  void *clientp UNUSED)
{
    return CURLKHSTAT_FINE;
}

static void apply_common_options(CURL *handle, CurlMountData *data)
{
    const char *environment;

    /* Never let libcurl install signal handlers: uvroot is a tracer and
     * the guest owns the signal state.  */
    curl_api.easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_api.easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_api.easy_setopt(handle, CURLOPT_USERAGENT, "uvroot-netfs/" VERSION);
    curl_api.easy_setopt(handle, CURLOPT_FTP_USE_EPSV, 1L);

    if (data->use_ssl != CURLUSESSL_NONE)
	curl_api.easy_setopt(handle, CURLOPT_USE_SSL, data->use_ssl);

    environment = getenv("UVROOT_NETFS_SSH_KNOWN_HOSTS");
    if (environment != NULL && environment[0] != '\0' && data->sftp)
	curl_api.easy_setopt(handle, CURLOPT_SSH_KNOWNHOSTS, environment);

    if (getenv("UVROOT_NETFS_INSECURE") != NULL) {
	curl_api.easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_api.easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);

	if (data->sftp) {
	    curl_api.easy_setopt(handle, CURLOPT_SSH_KEYFUNCTION,
				 ssh_key_accept);
	    curl_api.easy_setopt(handle, CURLOPT_SSH_KEYDATA, NULL);
	}
    }
}

/* ------------------------------------------------------------------ */
/* Directory listing                                                   */
/* ------------------------------------------------------------------ */

typedef struct ListContext {
    TALLOC_CTX *context;
    NetfsDirent *entries;
    size_t count;
    size_t capacity;
} ListContext;

/*
 * UTC mktime(3).  Kept local so the code does not depend on timegm(3),
 * which is missing from some C libraries used on embedded/Android
 * targets.
 */
static time_t netfs_timegm(const struct tm *parts)
{
    int64_t year = parts->tm_year + 1900;
    int64_t month = parts->tm_mon + 1;	/* 1..12 */
    int64_t day = parts->tm_mday;
    int64_t era;
    int64_t year_of_era;
    int64_t day_of_year;
    int64_t day_of_era;
    int64_t days;

    /* Howard Hinnant's days_from_civil algorithm.  */
    year -= month <= 2;
    era = (year >= 0 ? year : year - 399) / 400;
    year_of_era = year - era * 400;
    day_of_year =
	(153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    day_of_era = year_of_era * 365 + year_of_era / 4
	- year_of_era / 100 + day_of_year;
    days = era * 146097 + day_of_era - 719468;

    return (time_t) (days * 86400 + parts->tm_hour * 3600
		     + parts->tm_min * 60 + parts->tm_sec);
}

static time_t parse_remote_time(const char *text)
{
    struct tm parts;
    int year, month, day, hour, minute, second;

    if (text == NULL || text[0] == '\0')
	return 0;

    /* MLSD style: YYYYMMDDHHMMSS[.sss].  */
    if (sscanf(text, "%4d%2d%2d%2d%2d%2d", &year, &month, &day,
	       &hour, &minute, &second) == 6) {
	memset(&parts, 0, sizeof(parts));
	parts.tm_year = year - 1900;
	parts.tm_mon = month - 1;
	parts.tm_mday = day;
	parts.tm_hour = hour;
	parts.tm_min = minute;
	parts.tm_sec = second;
	return netfs_timegm(&parts);
    }

    if (curl_api.getdate != NULL)
	return curl_api.getdate(text, NULL);

    return 0;
}

static long list_chunk_begin(const void *transfer_info, void *userdata,
			     int remains UNUSED)
{
    const struct curl_fileinfo *info = transfer_info;
    ListContext *list = userdata;
    NetfsDirent *entry;
    size_t i;

    if (info->filename == NULL || info->filename[0] == '\0')
	return CURL_CHUNK_BGN_FUNC_SKIP;

    /* Guard against a recursive wildcard walk.  */
    if (strchr(info->filename, '/') != NULL)
	return CURL_CHUNK_BGN_FUNC_SKIP;

    for (i = 0; i < list->count; i++) {
	if (strcmp(list->entries[i].name, info->filename) == 0)
	    return CURL_CHUNK_BGN_FUNC_SKIP;
    }

    if (list->count == list->capacity) {
	size_t capacity = list->capacity == 0 ? 16 : list->capacity * 2;
	NetfsDirent *grown;

	grown = talloc_realloc(list->context, list->entries, NetfsDirent,
			       capacity);
	if (grown == NULL)
	    return CURL_CHUNK_BGN_FUNC_FAIL;
	list->entries = grown;
	list->capacity = capacity;
    }

    entry = &list->entries[list->count];
    memset(entry, 0, sizeof(*entry));
    entry->name = talloc_strdup(list->context, info->filename);
    if (entry->name == NULL)
	return CURL_CHUNK_BGN_FUNC_FAIL;

    entry->is_dir = (info->filetype == CURLFILETYPE_DIRECTORY);
    entry->is_link = (info->filetype == CURLFILETYPE_SYMLINK);
    if (entry->is_link && info->strings.target != NULL)
	entry->target = talloc_strdup(list->context, info->strings.target);
    entry->size = (info->flags & CURLFINFOFLAG_KNOWN_SIZE)
	? (off_t) info->size : 0;
    entry->mtime = parse_remote_time(info->strings.time);

    /* Preserve the remote permission bits: they decide whether the
     * cached file is executable, which matters when the mount is the
     * guest rootfs.  */
    if (info->flags & CURLFINFOFLAG_KNOWN_PERM)
	entry->mode = (info->perm & 07777)
	    | (entry->is_dir ? S_IFDIR : S_IFREG);
    else
	entry->mode = entry->is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);

    list->count++;
    return CURL_CHUNK_BGN_FUNC_SKIP;
}

static int curl_list_wildcard(NetfsMount *mount, const char *rel,
			      NetfsDirent **entries, size_t *count,
			      TALLOC_CTX *context)
{
    CurlMountData *data = mount->backend_data;
    ListContext list;
    char url[PATH_MAX * 3];
    CURL *handle;
    CURLcode code;

    handle = curl_api.easy_init();
    if (handle == NULL)
	return -ENOMEM;

    memset(&list, 0, sizeof(list));
    list.context = context;

    build_url(mount, rel, true, url, sizeof(url));
    strncat(url, "*", sizeof(url) - strlen(url) - 1);

    curl_api.easy_setopt(handle, CURLOPT_URL, url);
    curl_api.easy_setopt(handle, CURLOPT_WILDCARDMATCH, 1L);
    curl_api.easy_setopt(handle, CURLOPT_CHUNK_BGN_FUNCTION, list_chunk_begin);
    curl_api.easy_setopt(handle, CURLOPT_CHUNK_DATA, &list);
    apply_common_options(handle, data);

    code = curl_api.easy_perform(handle);
    curl_api.easy_cleanup(handle);

    /*
     * A wildcard listing of an *empty* directory makes libcurl report
     * "remote file not found" because the pattern matches nothing.  That
     * is not an error here: the dispatcher falls back to the raw text
     * listing, which reports a real failure.  So do not log this one.
     */
    if (code != CURLE_OK)
	return curl_code_to_errno(code);

    *entries = list.entries;
    *count = list.count;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Text listing parser (SFTP, and FTP fallback)                        */
/* ------------------------------------------------------------------ */

static bool next_token(const char **cursor, char *token, size_t size)
{
    const char *start;

    while (**cursor == ' ' || **cursor == '\t')
	(*cursor)++;
    if (**cursor == '\0')
	return false;

    start = *cursor;
    while (**cursor != '\0' && **cursor != ' ' && **cursor != '\t')
	(*cursor)++;

    {
	size_t length = (size_t) (*cursor - start);

	if (length >= size)
	    length = size - 1;
	memcpy(token, start, length);
	token[length] = '\0';
    }

    return true;
}

/* Convert the nine "rwxrwxrwx" characters of an "ls -l" listing into a
 * mode.  Permission bits must be preserved because they decide whether
 * a cached file is executable.  */
static mode_t mode_from_perms(const char *perms, bool is_dir)
{
    static const mode_t bits[9] = {
	S_IRUSR, S_IWUSR, S_IXUSR,
	S_IRGRP, S_IWGRP, S_IXGRP,
	S_IROTH, S_IWOTH, S_IXOTH
    };
    mode_t mode = is_dir ? S_IFDIR : S_IFREG;
    int i;

    for (i = 0; i < 9; i++) {
	if (perms[1 + i] == '\0')
	    break;
	if (perms[1 + i] != '-')
	    mode |= bits[i];
    }

    return mode;
}

static int month_number(const char *name)
{
    static const char *const months[] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    size_t i;

    for (i = 0; i < 12; i++) {
	if (strncasecmp(name, months[i], 3) == 0)
	    return (int) i;
    }

    return -1;
}

/*
 * Parse the date of a "ls -l" style listing.  Both the "Mon DD HH:MM"
 * (FTP) and "DD Mon HH:MM" (SFTP) orders are accepted, as well as the
 * year variants.  Timestamps are interpreted as UTC.
 */
static time_t parse_ls_date(const char *first, const char *second,
			    const char *third)
{
    struct tm parts;
    int month;
    int day;
    int year;
    int hour = 0;
    int minute = 0;
    int second_field = 0;
    time_t now;

    if (first == NULL || second == NULL || third == NULL)
	return 0;

    month = month_number(first);
    if (month >= 0) {
	day = atoi(second);
    } else {
	month = month_number(second);
	if (month < 0)
	    return 0;
	day = atoi(first);
    }

    if (strchr(third, ':') != NULL) {
	if (sscanf(third, "%d:%d:%d", &hour, &minute, &second_field) < 2)
	    return 0;
	now = time(NULL);
	{
	    struct tm *utc = gmtime(&now);
	    year = utc != NULL ? utc->tm_year + 1900 : 1970;
	}
    } else {
	year = atoi(third);
	if (year < 1970)
	    return 0;
    }

    memset(&parts, 0, sizeof(parts));
    parts.tm_year = year - 1900;
    parts.tm_mon = month;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    parts.tm_min = minute;
    parts.tm_sec = second_field;
    parts.tm_isdst = -1;

    /*
     * "ls -l" style listings carry the server's local time, so it is
     * interpreted locally; MLSD timestamps (parse_remote_time) are UTC
     * by definition and use timegm().
     */
    return mktime(&parts);
}

/*
 * Parse one line of a directory listing.  Handles both the MLSD
 * "type=file;size=12;modify=20230101120000; name" form and the
 * traditional "ls -l" form produced by FTP LIST and by libcurl's SFTP
 * directory transfer.
 */
static void parse_list_line(const char *line, NetfsDirent *entry,
			    TALLOC_CTX *context)
{
    const char *cursor = line;

    while (*cursor == ' ' || *cursor == '\t')
	cursor++;
    if (*cursor == '\0' || strncmp(cursor, "total ", 6) == 0)
	return;

    entry->mode = S_IFREG | 0644;

    if (strchr(cursor, '=') != NULL && strchr(cursor, ';') != NULL) {
	/* MLSD.  */
	const char *name = NULL;
	const char *field = cursor;
	mode_t perm_bits = 0;
	bool have_perm = false;

	while (*field != '\0' && *field != ' ') {
	    const char *semicolon = strchr(field, ';');

	    if (semicolon == NULL)
		break;
	    if (strncmp(field, "type=", 5) == 0) {
		if (strncmp(field + 5, "dir", 3) == 0)
		    entry->is_dir = true;
		else if (strncmp(field + 5, "OS.unix=slink", 13) == 0) {
		    /* type=OS.unix=slink:/target */
		    const char *value = field + 5 + 13;

		    entry->is_link = true;
		    if (*value == ':')
			value++;
		    if (*value != '\0')
			entry->target = talloc_strdup(context, value);
		}
	    } else if (strncmp(field, "size=", 5) == 0) {
		entry->size = (off_t) strtoll(field + 5, NULL, 10);
	    } else if (strncmp(field, "modify=", 7) == 0) {
		entry->mtime = parse_remote_time(field + 7);
	    } else if (strncmp(field, "UNIX.mode=", 10) == 0
		       || strncmp(field, "perm=", 5) == 0) {
		const char *value = field + (field[0] == 'U' ? 10 : 5);

		perm_bits = (mode_t) strtol(value, NULL, 8) & 07777;
		have_perm = true;
	    }
	    field = semicolon + 1;
	}

	while (*field == ' ')
	    field++;
	name = field;
	if (name[0] == '\0')
	    return;
	entry->name = talloc_strdup(context, name);
	entry->mode = (have_perm ? perm_bits
		       : (entry->is_dir ? 0755 : 0644))
	    | (entry->is_dir ? S_IFDIR : S_IFREG);
	return;
    }

    {
	char perms[32], links[32], owner[64], group[64], size[32];
	char first[32], second[32], third[32];
	const char *remainder;

	if (!next_token(&cursor, perms, sizeof(perms))
	    || !next_token(&cursor, links, sizeof(links))
	    || !next_token(&cursor, owner, sizeof(owner))
	    || !next_token(&cursor, group, sizeof(group))
	    || !next_token(&cursor, size, sizeof(size))
	    || !next_token(&cursor, first, sizeof(first))
	    || !next_token(&cursor, second, sizeof(second))
	    || !next_token(&cursor, third, sizeof(third)))
	    return;

	remainder = cursor;
	while (*remainder == ' ' || *remainder == '\t')
	    remainder++;
	if (*remainder == '\0')
	    return;

	entry->is_dir = (perms[0] == 'd');
	entry->is_link = (perms[0] == 'l');
	entry->mode = mode_from_perms(perms, entry->is_dir);
	entry->size = (off_t) strtoll(size, NULL, 10);
	entry->mtime = parse_ls_date(first, second, third);

	{
	    const char *arrow = strstr(remainder, " -> ");
	    size_t length = arrow != NULL
		? (size_t) (arrow - remainder) : strlen(remainder);

	    if (length == 0)
		return;
	    entry->name = talloc_strndup(context, remainder, length);
	    if (arrow != NULL && arrow[4] != '\0')
		entry->target = talloc_strdup(context, arrow + 4);
	}
    }
}

typedef struct TextList {
    TALLOC_CTX *context;
    char *data;
    size_t length;
    size_t capacity;
} TextList;

static size_t append_list_data(void *ptr, size_t size, size_t nmemb,
			       void *userdata)
{
    TextList *list = userdata;
    size_t bytes = size * nmemb;

    if (list->length + bytes + 1 > list->capacity) {
	size_t capacity = list->capacity == 0 ? 4096 : list->capacity;
	char *grown;

	while (capacity < list->length + bytes + 1)
	    capacity *= 2;
	grown = talloc_realloc(list->context, list->data, char, capacity);
	if (grown == NULL)
	    return 0;
	list->data = grown;
	list->capacity = capacity;
    }

    memcpy(list->data + list->length, ptr, bytes);
    list->length += bytes;
    list->data[list->length] = '\0';

    return bytes;
}

static int curl_list_text(NetfsMount *mount, const char *rel,
			  NetfsDirent **entries, size_t *count,
			  TALLOC_CTX *context)
{
    CurlMountData *data = mount->backend_data;
    TextList list;
    NetfsDirent *array = NULL;
    size_t used = 0;
    char url[PATH_MAX * 3];
    char *line;
    char *save;
    CURL *handle;
    CURLcode code;
    int status;

    handle = curl_api.easy_init();
    if (handle == NULL)
	return -ENOMEM;

    memset(&list, 0, sizeof(list));
    list.context = context;

    build_url(mount, rel, true, url, sizeof(url));

    curl_api.easy_setopt(handle, CURLOPT_URL, url);
    curl_api.easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_list_data);
    curl_api.easy_setopt(handle, CURLOPT_WRITEDATA, &list);
    apply_common_options(handle, data);

    code = curl_api.easy_perform(handle);
    curl_api.easy_cleanup(handle);

    if (code != CURLE_OK)
	return report_curl_error("list", url, code);

    if (list.data == NULL) {
	*entries = NULL;
	*count = 0;
	return 0;
    }

    for (line = strtok_r(list.data, "\r\n", &save); line != NULL;
	 line = strtok_r(NULL, "\r\n", &save)) {
	NetfsDirent candidate;

	memset(&candidate, 0, sizeof(candidate));
	parse_list_line(line, &candidate, context);
	if (candidate.name == NULL || candidate.name[0] == '\0')
	    continue;

	{
	    NetfsDirent *grown;

	    grown = talloc_realloc(context, array, NetfsDirent, used + 1);
	    if (grown == NULL) {
		status = -ENOMEM;
		goto out;
	    }
	    array = grown;
	}
	array[used++] = candidate;
    }

    *entries = array;
    *count = used;
    status = 0;

  out:
    talloc_free(list.data);
    return status;
}

/*
 * Directory listing dispatcher.  libcurl's wildcard API understands
 * FTP MLSD/LIST, but it does not work over SFTP (it tries to stat the
 * literal "slash-star" path), so SFTP uses the raw directory transfer
 * and a longname parser.  The same parser is the fallback whenever the
 * wildcard listing fails.
 */
static int curl_list(NetfsMount *mount, const char *rel, NetfsDirent **entries,
		     size_t *count, TALLOC_CTX *context)
{
    CurlMountData *data = mount->backend_data;
    int status;

    if (data->sftp)
	return curl_list_text(mount, rel, entries, count, context);

    status =
	curl_list_wildcard(mount, rel, entries, count, context);
    if (status < 0)
	return curl_list_text(mount, rel, entries, count, context);

    return status;
}

static int curl_stat(NetfsMount *mount, const char *rel, struct stat *st)
{
    NetfsDirent *entries = NULL;
    size_t count = 0;
    TALLOC_CTX *context;
    const char *base;
    size_t i;
    int status;
    char parent[PATH_MAX];

    if (rel[0] == '\0') {
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFDIR | 0755;
	return 0;
    }

    snprintf(parent, sizeof(parent), "%s", rel);
    base = strrchr(parent, '/');
    if (base != NULL)
	*(char *) base++ = '\0';
    else
	base = rel;

    context = talloc_new(NULL);
    if (context == NULL)
	return -ENOMEM;

    status = curl_list(mount, base == rel ? "" : parent, &entries, &count,
		       context);
    if (status < 0) {
	talloc_free(context);
	return status;
    }

    for (i = 0; i < count; i++) {
	if (strcmp(entries[i].name, base) == 0) {
	    memset(st, 0, sizeof(*st));
	    st->st_mode = entries[i].is_dir
		? (S_IFDIR | 0755) : (S_IFREG | 0644);
	    st->st_size = entries[i].size;
	    st->st_mtime = entries[i].mtime;
	    talloc_free(context);
	    return 0;
	}
    }

    talloc_free(context);
    return -ENOENT;
}

/* ------------------------------------------------------------------ */
/* Content transfer                                                    */
/* ------------------------------------------------------------------ */

static int curl_get(NetfsMount *mount, const char *rel, const char *local)
{
    CurlMountData *data = mount->backend_data;
    char url[PATH_MAX * 3];
    CURL *handle;
    CURLcode code;
    FILE *file;

    file = fopen(local, "wb");
    if (file == NULL)
	return -errno;

    handle = curl_api.easy_init();
    if (handle == NULL) {
	fclose(file);
	return -ENOMEM;
    }

    build_url(mount, rel, false, url, sizeof(url));

    curl_api.easy_setopt(handle, CURLOPT_URL, url);
    curl_api.easy_setopt(handle, CURLOPT_WRITEDATA, file);
    apply_common_options(handle, data);

    code = curl_api.easy_perform(handle);
    curl_api.easy_cleanup(handle);
    if (fclose(file) != 0 && code == CURLE_OK)
	return -EIO;

    return report_curl_error("read", url, code);
}

static int curl_put(NetfsMount *mount, const char *rel, const char *local)
{
    CurlMountData *data = mount->backend_data;
    char url[PATH_MAX * 3];
    CURL *handle;
    CURLcode code;
    FILE *file;
    struct stat st;

    if (stat(local, &st) < 0)
	return -errno;
    if (!S_ISREG(st.st_mode))
	return -EISDIR;

    file = fopen(local, "rb");
    if (file == NULL)
	return -errno;

    handle = curl_api.easy_init();
    if (handle == NULL) {
	fclose(file);
	return -ENOMEM;
    }

    build_url(mount, rel, false, url, sizeof(url));

    curl_api.easy_setopt(handle, CURLOPT_URL, url);
    curl_api.easy_setopt(handle, CURLOPT_UPLOAD, 1L);
    curl_api.easy_setopt(handle, CURLOPT_READDATA, file);
    curl_api.easy_setopt(handle, CURLOPT_INFILESIZE_LARGE,
			 (curl_off_t) st.st_size);
    apply_common_options(handle, data);

    code = curl_api.easy_perform(handle);
    curl_api.easy_cleanup(handle);
    fclose(file);

    return report_curl_error("write", url, code);
}

/* ------------------------------------------------------------------ */
/* Namespace operations                                                */
/* ------------------------------------------------------------------ */

static int run_quotes(NetfsMount *mount, const char *rel,
		      struct curl_slist *quotes)
{
    CurlMountData *data = mount->backend_data;
    char url[PATH_MAX * 3];
    CURL *handle;
    CURLcode code;

    handle = curl_api.easy_init();
    if (handle == NULL)
	return -ENOMEM;

    build_parent_url(mount, rel, url, sizeof(url));

    curl_api.easy_setopt(handle, CURLOPT_URL, url);
    curl_api.easy_setopt(handle, CURLOPT_QUOTE, quotes);
    curl_api.easy_setopt(handle, CURLOPT_WRITEFUNCTION, discard_write);
    apply_common_options(handle, data);

    code = curl_api.easy_perform(handle);
    curl_api.easy_cleanup(handle);

    return report_curl_error("quote", url, code);
}

static int curl_mkdir(NetfsMount *mount, const char *rel)
{
    CurlMountData *data = mount->backend_data;
    char path[PATH_MAX * 2];
    char command[PATH_MAX * 2 + 16];
    struct curl_slist *quotes;
    int status;

    remote_abspath(mount, rel, path, sizeof(path));
    snprintf(command, sizeof(command), data->sftp ? "mkdir %s" : "MKD %s",
	     path);

    quotes = curl_api.slist_append(NULL, command);
    if (quotes == NULL)
	return -ENOMEM;
    status = run_quotes(mount, rel, quotes);
    curl_api.slist_free_all(quotes);

    return status;
}

static int curl_rmdir(NetfsMount *mount, const char *rel)
{
    CurlMountData *data = mount->backend_data;
    char path[PATH_MAX * 2];
    char command[PATH_MAX * 2 + 16];
    struct curl_slist *quotes;
    int status;

    remote_abspath(mount, rel, path, sizeof(path));
    snprintf(command, sizeof(command), data->sftp ? "rmdir %s" : "RMD %s",
	     path);

    quotes = curl_api.slist_append(NULL, command);
    if (quotes == NULL)
	return -ENOMEM;
    status = run_quotes(mount, rel, quotes);
    curl_api.slist_free_all(quotes);

    return status;
}

static int curl_unlink(NetfsMount *mount, const char *rel)
{
    CurlMountData *data = mount->backend_data;
    char path[PATH_MAX * 2];
    char command[PATH_MAX * 2 + 16];
    struct curl_slist *quotes;
    int status;

    remote_abspath(mount, rel, path, sizeof(path));
    snprintf(command, sizeof(command), data->sftp ? "rm %s" : "DELE %s", path);

    quotes = curl_api.slist_append(NULL, command);
    if (quotes == NULL)
	return -ENOMEM;
    status = run_quotes(mount, rel, quotes);
    curl_api.slist_free_all(quotes);

    return status;
}

static int curl_rename(NetfsMount *mount, const char *from, const char *to)
{
    CurlMountData *data = mount->backend_data;
    char old_path[PATH_MAX * 2];
    char new_path[PATH_MAX * 2];
    char command[PATH_MAX * 4 + 32];
    struct curl_slist *quotes = NULL;
    struct curl_slist *item;
    int status;

    remote_abspath(mount, from, old_path, sizeof(old_path));
    remote_abspath(mount, to, new_path, sizeof(new_path));

    if (data->sftp) {
	snprintf(command, sizeof(command), "rename %s %s", old_path,
		 new_path);
	item = curl_api.slist_append(NULL, command);
	if (item == NULL)
	    return -ENOMEM;
	quotes = item;
    } else {
	snprintf(command, sizeof(command), "RNFR %s", old_path);
	item = curl_api.slist_append(NULL, command);
	if (item == NULL)
	    return -ENOMEM;
	quotes = item;

	snprintf(command, sizeof(command), "RNTO %s", new_path);
	item = curl_api.slist_append(quotes, command);
	if (item == NULL) {
	    curl_api.slist_free_all(quotes);
	    return -ENOMEM;
	}
	quotes = item;
    }

    status = run_quotes(mount, from, quotes);
    curl_api.slist_free_all(quotes);

    return status;
}

/* ------------------------------------------------------------------ */
/* Backend plumbing                                                    */
/* ------------------------------------------------------------------ */

static void curl_fini(NetfsMount *mount)
{
    CurlMountData *data = mount->backend_data;

    if (data != NULL)
	TALLOC_FREE(data);
    mount->backend_data = NULL;
}

static int curl_init(NetfsMount *mount, const char *url)
{
    CurlMountData *data;
    const char *marker;
    const char *path;
    char *base;
    size_t length;

    if (!load_curl_api(NULL)) {
	note(NULL, WARNING, USER,
	     "netfs: this build has libcurl support but libcurl could not "
	     "be loaded at run time (set UVROOT_NETFS_LIBCURL to its path)");
	return -ENOSYS;
    }

    data = talloc_zero(mount, CurlMountData);
    if (data == NULL)
	return -ENOMEM;

    data->sftp = (strcasecmp(mount->scheme, "sftp") == 0);

    if (strcasecmp(mount->scheme, "ftpes") == 0) {
	/* Explicit TLS: ftpes:// is not a libcurl scheme, it is the
	 * plain FTP scheme with mandatory STARTTLS.  */
	base = talloc_asprintf(data, "ftp://%s", url + strlen("ftpes://"));
	data->use_ssl = CURLUSESSL_ALL;
    } else {
	base = talloc_strdup(data, url);
	data->use_ssl = CURLUSESSL_NONE;
    }

    if (base == NULL)
	return -ENOMEM;

    /* Strip any trailing slash so path components can be appended.  */
    length = strlen(base);
    while (length > 0 && base[length - 1] == '/' && length > strlen("ftp://"))
	base[--length] = '\0';

    data->base = base;

    /* Extract the path part, needed by the raw quote commands.  */
    marker = strstr(base, "://");
    path = marker != NULL ? strchr(marker + 3, '/') : NULL;
    data->remote_path = talloc_strdup(data, path != NULL ? path : "/");
    if (data->remote_path == NULL)
	return -ENOMEM;

    /* Normalize a trailing slash on the remote path.  */
    length = strlen(data->remote_path);
    while (length > 1 && data->remote_path[length - 1] == '/')
	data->remote_path[--length] = '\0';

    mount->backend_data = data;
    return 0;
}

const NetfsBackend netfs_backend_ftp = {
    .name = "ftp",
    .schemes = ftp_schemes,
    .kind = NETFS_KIND_DIR,
    .implemented = true,
    .dir = {
	    .list = curl_list,
	    .stat = curl_stat,
	    .get = curl_get,
	    .put = curl_put,
	    .mkdir = curl_mkdir,
	    .rmdir = curl_rmdir,
	    .unlink = curl_unlink,
	    .rename = curl_rename,
	    },
    .init = curl_init,
    .fini = curl_fini,
};

#else				/* !HAVE_LIBCURL */

const NetfsBackend netfs_backend_ftp = {
    .name = "ftp",
    .schemes = ftp_schemes,
    .kind = NETFS_KIND_DIR,
    .implemented = false,
    .reserved_note = "this build was made without libcurl headers",
};

#endif				/* HAVE_LIBCURL */
