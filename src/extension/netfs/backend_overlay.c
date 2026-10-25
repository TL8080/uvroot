/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Overlay directory backend.
 *
 * The backend presents a merged view of a writable upper tree (type 1)
 * and a read-only lower tree (type 2).  It sits below the generic netfs
 * cache, which gives the guest the usual lazy materialization and
 * write-back path.  The .overlay database in the upper root records
 * whiteouts and, in block mode, the sparse delta of every modified
 * file.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <talloc.h>

#include "extension/netfs/netfs.h"
#include "extension/overlay/overlay_db.h"

typedef struct OverlayData {
    OverlayDb db;
    pthread_mutex_t lock;

    /* type 1 may be a raw/QCOW2/NBD/... source; local @db.upper is the
     * materialized staging tree and these operations push each change
     * back to that source.  */
    NetfsMount *upper_source;
    const NetfsDirOps *upper_ops;
} OverlayData;

static OverlayData *overlay_data(NetfsMount *mount)
{
    return mount->backend_data;
}

/* The database and its temporary files are not part of the merged view.  */
static bool overlay_private(const char *rel)
{
    static const char vperm_name[] = ".uvroot-vperm";

    if (strcmp(rel, OVERLAY_DB_NAME) == 0)
	return true;
    if (strncmp(rel, OVERLAY_DB_NAME, strlen(OVERLAY_DB_NAME)) == 0) {
	char next = rel[strlen(OVERLAY_DB_NAME)];

	return next == '/' || next == '.';
    }
    /* vperm keeps its database in the writable upper; never expose it. */
    if (strncmp(rel, vperm_name, sizeof(vperm_name) - 1) == 0) {
	char next = rel[sizeof(vperm_name) - 1];

	return next == '\0' || next == '/' || next == '.';
    }
    return strncmp(rel, ".bkc-check", sizeof(".bkc-check") - 1) == 0
	|| strncmp(rel, ".bme-verify", sizeof(".bme-verify") - 1) == 0;
}

static int ensure_parent(OverlayData *data, const char *rel)
{
    char upper[OVERLAY_PATH_MAX];
    char lower[OVERLAY_PATH_MAX];
    char prefix[OVERLAY_PATH_MAX];
    const char *cursor;
    size_t used;

    if (snprintf(upper, sizeof(upper), "%s", data->db.upper)
	>= (int) sizeof(upper))
	return -ENAMETOOLONG;
    used = strlen(upper);
    prefix[0] = '\0';

    for (cursor = rel; *cursor != '\0';) {
	const char *slash = strchr(cursor, '/');
	size_t length = slash != NULL ? (size_t) (slash - cursor)
	    : strlen(cursor);
	size_t prefix_length = strlen(prefix);

	if (slash == NULL)
	    break;			/* the final component is the object */
	if (used + 1 + length >= sizeof(upper))
	    return -ENAMETOOLONG;
	upper[used++] = '/';
	memcpy(upper + used, cursor, length);
	used += length;
	upper[used] = '\0';

	if (prefix_length + 1 + length >= sizeof(prefix))
	    return -ENAMETOOLONG;
	if (prefix_length != 0)
	    prefix[prefix_length++] = '/';
	memcpy(prefix + prefix_length, cursor, length);
	prefix[prefix_length + length] = '\0';

	if (mkdir(upper, 0755) < 0) {
	    if (errno != EEXIST)
		return -errno;
	} else if (overlay_join(lower, sizeof(lower), data->db.lower,
				prefix) == 0) {
	    struct stat st;

	    if (lstat(lower, &st) == 0 && S_ISDIR(st.st_mode))
		(void) chmod(upper, st.st_mode & 07777);
	}
	cursor = slash + 1;
    }
    return 0;
}


static int source_put(OverlayData *data, const char *rel, const char *local)
{
    if (data->upper_source == NULL)
	return 0;
    if (data->upper_ops == NULL || data->upper_ops->put == NULL)
	return -ENOSYS;
    return data->upper_ops->put(data->upper_source, rel, local);
}

static int source_unlink(OverlayData *data, const char *rel)
{
    int status;

    if (data->upper_source == NULL)
	return 0;
    if (data->upper_ops == NULL || data->upper_ops->unlink == NULL)
	return -ENOSYS;
    status = data->upper_ops->unlink(data->upper_source, rel);
    return status == -ENOENT ? 0 : status;
}

static int source_mkdir(OverlayData *data, const char *rel)
{
    int status;

    if (data->upper_source == NULL)
	return 0;
    if (data->upper_ops == NULL || data->upper_ops->mkdir == NULL)
	return -ENOSYS;
    status = data->upper_ops->mkdir(data->upper_source, rel);
    return status == -EEXIST ? 0 : status;
}

static int source_rmdir(OverlayData *data, const char *rel)
{
    int status;

    if (data->upper_source == NULL)
	return 0;
    if (data->upper_ops == NULL || data->upper_ops->rmdir == NULL)
	return -ENOSYS;
    status = data->upper_ops->rmdir(data->upper_source, rel);
    return status == -ENOENT ? 0 : status;
}

static int source_symlink(OverlayData *data, const char *rel,
			  const char *target)
{
    if (data->upper_source == NULL)
	return 0;
    if (data->upper_ops == NULL || data->upper_ops->symlink == NULL)
	return -ENOSYS;
    return data->upper_ops->symlink(data->upper_source, rel, target);
}

static int source_link(OverlayData *data, const char *from, const char *to)
{
    if (data->upper_source == NULL)
	return 0;
    if (data->upper_ops == NULL || data->upper_ops->link == NULL)
	return -ENOSYS;
    return data->upper_ops->link(data->upper_source, from, to);
}

/*
 * Persist the local database and, for a non-directory type 1, push the
 * very same database into the source filesystem.
 */
static int overlay_commit(OverlayData *data)
{
    int status = overlay_db_save(&data->db);

    if (status < 0)
	return status;
    return source_put(data, OVERLAY_DB_NAME, data->db.path);
}

static int remove_upper_tree(const char *path)
{
    struct stat st;

    if (lstat(path, &st) < 0)
	return errno == ENOENT ? 0 : -errno;
    if (S_ISDIR(st.st_mode)) {
	DIR *dir = opendir(path);
	struct dirent *entry;
	int status = 0;

	if (dir == NULL)
	    return -errno;
	while ((entry = readdir(dir)) != NULL) {
	    char child[OVERLAY_PATH_MAX];

	    if (strcmp(entry->d_name, ".") == 0
		|| strcmp(entry->d_name, "..") == 0)
		continue;
	    if (snprintf(child, sizeof(child), "%s/%s", path,
			 entry->d_name) >= (int) sizeof(child)) {
		status = -ENAMETOOLONG;
		break;
	    }
	    status = remove_upper_tree(child);
	    if (status < 0)
		break;
	}
	closedir(dir);
	if (status < 0)
	    return status;
	return rmdir(path) < 0 ? -errno : 0;
    }
    return unlink(path) < 0 ? -errno : 0;
}

static int effective_lstat(OverlayData *data, const char *rel,
			   struct stat *st, bool *upper)
{
    char path[OVERLAY_PATH_MAX];
    OverlayEntry *entry;

    if (overlay_private(rel) || overlay_db_hidden(&data->db, rel))
	return -ENOENT;
    entry = overlay_entry_find(&data->db, rel);
    if (entry != NULL && entry->block) {
	memset(st, 0, sizeof(*st));
	st->st_mode = entry->mode;
	if ((st->st_mode & S_IFMT) == 0)
	    st->st_mode |= S_IFREG;
	st->st_size = (off_t) entry->size;
	st->st_mtime = entry->mtime;
	st->st_uid = 0;
	st->st_gid = 0;
	st->st_nlink = 1;
	if (upper != NULL)
	    *upper = true;
	return 0;
    }
    if (overlay_join(path, sizeof(path), data->db.upper, rel) < 0)
	return -ENAMETOOLONG;
    if (lstat(path, st) == 0) {
	if (upper != NULL)
	    *upper = true;
	return 0;
    }
    if (errno != ENOENT)
	return -errno;
    if (overlay_join(path, sizeof(path), data->db.lower, rel) < 0)
	return -ENAMETOOLONG;
    if (lstat(path, st) == 0) {
	if (upper != NULL)
	    *upper = false;
	return 0;
    }
    return errno == ENOENT ? -ENOENT : -errno;
}

static int add_dirent(NetfsDirent **entries, size_t *count, size_t *capacity,
		      TALLOC_CTX *ctx, const char *name, const char *rel,
		      OverlayData *data)
{
    struct stat st;
    NetfsDirent *grown;
    NetfsDirent *item;
    bool upper;

    if (effective_lstat(data, rel, &st, &upper) < 0)
	return 0;
    if (*count == *capacity) {
	size_t next = *capacity == 0 ? 16 : *capacity * 2;

	grown = talloc_realloc(ctx, *entries, NetfsDirent, next);
	if (grown == NULL)
	    return -ENOMEM;
	*entries = grown;
	*capacity = next;
    }
    item = &(*entries)[*count];
    memset(item, 0, sizeof(*item));
    item->name = talloc_strdup(ctx, name);
    if (item->name == NULL)
	return -ENOMEM;
    item->is_dir = S_ISDIR(st.st_mode);
    item->is_link = S_ISLNK(st.st_mode);
    item->size = st.st_size;
    item->mtime = st.st_mtime;
    item->mode = st.st_mode;
    item->ino = (uint64_t) st.st_ino;
    if (item->is_link) {
	char path[OVERLAY_PATH_MAX];
	char target[OVERLAY_PATH_MAX];
	ssize_t got;

	if (overlay_join(path, sizeof(path),
			 upper ? data->db.upper : data->db.lower,
			 rel) == 0) {
	    got = readlink(path, target, sizeof(target) - 1);
	    if (got >= 0) {
		target[got] = '\0';
		item->target = talloc_strdup(ctx, target);
	    }
	}
    }
    (*count)++;
    return 0;
}

static int collect_dir(NetfsDirent **entries, size_t *count,
		       size_t *capacity, TALLOC_CTX *ctx,
		       OverlayData *data, const char *rel, const char *base,
		       bool upper)
{
    char directory[OVERLAY_PATH_MAX];
    DIR *dir;
    struct dirent *item;

    if (overlay_join(directory, sizeof(directory), base, rel) < 0)
	return -ENAMETOOLONG;
    dir = opendir(directory);
    if (dir == NULL) {
	if (errno == ENOENT)
	    return 0;
	return -errno;
    }
    while ((item = readdir(dir)) != NULL) {
	char child_rel[OVERLAY_PATH_MAX];
	size_t i;
	bool seen = false;

	if (strcmp(item->d_name, ".") == 0
	    || strcmp(item->d_name, "..") == 0)
	    continue;
	if (rel[0] == '\0') {
	    if (snprintf(child_rel, sizeof(child_rel), "%s", item->d_name)
		>= (int) sizeof(child_rel))
		continue;
	} else if (overlay_join(child_rel, sizeof(child_rel), rel,
				item->d_name) < 0) {
	    continue;
	}
	if (overlay_private(child_rel) || overlay_db_hidden(&data->db, child_rel))
	    continue;
	/* The upper listing wins over the lower listing.  */
	if (!upper) {
	    char upper_path[OVERLAY_PATH_MAX];

	    if (overlay_join(upper_path, sizeof(upper_path), data->db.upper,
			     child_rel) == 0
		&& lstat(upper_path, &(struct stat){ 0 }) == 0)
		continue;
	}
	for (i = 0; i < *count; i++) {
	    if (strcmp((*entries)[i].name, item->d_name) == 0) {
		seen = true;
		break;
	    }
	}
	if (seen)
	    continue;
	if (add_dirent(entries, count, capacity, ctx, item->d_name,
		       child_rel, data) < 0)
	    continue;
    }
    closedir(dir);
    return 0;
}

static int overlay_list(NetfsMount *mount, const char *rel,
			NetfsDirent **entries, size_t *count, TALLOC_CTX *ctx)
{
    OverlayData *data = overlay_data(mount);
    size_t capacity = 0;
    int status = 0;

    *entries = NULL;
    *count = 0;

    pthread_mutex_lock(&data->lock);
    if (overlay_private(rel) || overlay_db_hidden(&data->db, rel)) {
	pthread_mutex_unlock(&data->lock);
	return -ENOENT;
    }
    status = collect_dir(entries, count, &capacity, ctx, data, rel,
			 data->db.upper, true);
    if (status == 0)
	status = collect_dir(entries, count, &capacity, ctx, data, rel,
			     data->db.lower, false);
    pthread_mutex_unlock(&data->lock);
    return status;
}

static int overlay_stat(NetfsMount *mount, const char *rel, struct stat *st)
{
    OverlayData *data = overlay_data(mount);
    int status;

    pthread_mutex_lock(&data->lock);
    status = effective_lstat(data, rel, st, NULL);
    pthread_mutex_unlock(&data->lock);
    return status;
}

static int overlay_get(NetfsMount *mount, const char *rel, const char *local)
{
    OverlayData *data = overlay_data(mount);
    OverlayEntry *entry;
    char upper[OVERLAY_PATH_MAX];
    char lower[OVERLAY_PATH_MAX];
    struct stat st;
    int status;

    pthread_mutex_lock(&data->lock);
    if (overlay_private(rel)) {
	pthread_mutex_unlock(&data->lock);
	return -ENOENT;
    }
    entry = overlay_entry_find(&data->db, rel);
    if (entry != NULL && entry->block) {
	status = overlay_delta_merge(&data->db, entry, NULL, NULL, local);
	pthread_mutex_unlock(&data->lock);
	return status;
    }
    status = effective_lstat(data, rel, &st, NULL);
    if (status < 0) {
	pthread_mutex_unlock(&data->lock);
	return status;
    }
    if (S_ISDIR(st.st_mode)) {
	pthread_mutex_unlock(&data->lock);
	return -EISDIR;
    }
    if (overlay_join(upper, sizeof(upper), data->db.upper, rel) < 0
	|| overlay_join(lower, sizeof(lower), data->db.lower, rel) < 0) {
	pthread_mutex_unlock(&data->lock);
	return -ENAMETOOLONG;
    }
    if (lstat(upper, &st) == 0)
	status = overlay_copy_file(upper, local, st.st_mode);
    else
	status = overlay_copy_file(lower, local, st.st_mode);
    pthread_mutex_unlock(&data->lock);
    return status;
}

static int write_in_place(const char *local, const char *destination,
			  mode_t mode)
{
    unsigned char buffer[65536];
    ssize_t got;
    int in;
    int out;
    int status = 0;

    in = open(local, O_RDONLY | O_CLOEXEC);
    if (in < 0)
	return -errno;
    out = open(destination, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (out < 0) {
	status = -errno;
	close(in);
	return status;
    }
    while ((got = read(in, buffer, sizeof(buffer))) > 0) {
	ssize_t done = 0;

	while (done < got) {
	    ssize_t written = write(out, buffer + done,
				    (size_t) (got - done));

	    if (written < 0) {
		status = -errno;
		goto out;
	    }
	    done += written;
	}
    }
    if (got < 0)
	status = -errno;
    (void) fchmod(out, mode & 07777);
  out:
    close(in);
    if (close(out) < 0 && status == 0)
	status = -errno;
    return status;
}

static int store_full(OverlayData *data, const char *rel, const char *local)
{
    char upper[OVERLAY_PATH_MAX];
    struct stat st;
    int status;

    if (ensure_parent(data, rel) < 0)
	return -ENOENT;
    if (overlay_join(upper, sizeof(upper), data->db.upper, rel) < 0)
	return -ENAMETOOLONG;
    if (stat(local, &st) < 0)
	return -errno;
    {
	struct stat upper_st;

	if (lstat(upper, &upper_st) == 0 && S_ISREG(upper_st.st_mode)
	    && upper_st.st_nlink > 1)
	    status = write_in_place(local, upper, st.st_mode);
	else
	    status = overlay_copy_file(local, upper, st.st_mode);
    }
    if (status < 0)
	return status;
    overlay_entry_remove(&data->db, rel);
    status = overlay_commit(data);
    if (status < 0)
	return status;
    return source_put(data, rel, upper);
}

static int store_delta(OverlayData *data, const char *rel, const char *local)
{
    char lower[OVERLAY_PATH_MAX];
    char upper[OVERLAY_PATH_MAX];
    char temporary[OVERLAY_PATH_MAX + 64];
    unsigned char local_block[OVERLAY_BLOCK_SIZE];
    unsigned char lower_block[OVERLAY_BLOCK_SIZE];
    struct stat local_st;
    struct stat lower_st;
    OverlayEntry *entry;
    uint64_t offset;
    int in_local;
    int in_lower;
    int out;
    int status = 0;
    bool have_ranges = false;

    if (overlay_join(lower, sizeof(lower), data->db.lower, rel) < 0
	|| overlay_join(upper, sizeof(upper), data->db.upper, rel) < 0)
	return -ENAMETOOLONG;
    if (lstat(lower, &lower_st) < 0 || !S_ISREG(lower_st.st_mode))
	return store_full(data, rel, local);
    if (stat(local, &local_st) < 0)
	return -errno;
    if (ensure_parent(data, rel) < 0)
	return -ENOENT;

    if (snprintf(temporary, sizeof(temporary), "%s.delta.%ld", upper,
		 (long) getpid()) >= (int) sizeof(temporary))
	return -ENAMETOOLONG;
    unlink(temporary);
    out = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
	       (lower_st.st_mode & 07777) | S_IRUSR | S_IWUSR);
    if (out < 0)
	return -errno;
    in_local = open(local, O_RDONLY | O_CLOEXEC);
    in_lower = open(lower, O_RDONLY | O_CLOEXEC);
    if (in_local < 0 || in_lower < 0) {
	status = -errno;
	goto out;
    }

    /* Rebuild the delta from scratch: old ranges must not survive.  */
    entry = overlay_entry_get(&data->db, rel, true);
    if (entry == NULL) {
	status = -ENOMEM;
	goto out;
    }
    while (entry->ranges != NULL) {
	OverlayRange *next = entry->ranges->next;

	free(entry->ranges);
	entry->ranges = next;
    }
    entry->deleted = false;

    for (offset = 0; offset < (uint64_t) local_st.st_size;
	 offset += OVERLAY_BLOCK_SIZE) {
	size_t local_len = (size_t) ((uint64_t) local_st.st_size - offset);
	size_t lower_len = offset < (uint64_t) lower_st.st_size
	    ? (size_t) ((uint64_t) lower_st.st_size - offset) : 0;
	ssize_t got_local;
	ssize_t got_lower;
	bool changed;

	if (local_len > OVERLAY_BLOCK_SIZE)
	    local_len = OVERLAY_BLOCK_SIZE;
	if (lower_len > OVERLAY_BLOCK_SIZE)
	    lower_len = OVERLAY_BLOCK_SIZE;
	got_local = pread(in_local, local_block, local_len, (off_t) offset);
	if (got_local < 0) {
	    status = -errno;
	    goto out;
	}
	local_len = (size_t) got_local;
	got_lower = lower_len == 0 ? 0
	    : pread(in_lower, lower_block, lower_len, (off_t) offset);
	if (got_lower < 0) {
	    status = -errno;
	    goto out;
	}
	lower_len = (size_t) got_lower;
	changed = local_len != lower_len
	    || memcmp(local_block, lower_block, local_len) != 0;

	/*
	 * When the file shrank, the truncate below removes the trailing
	 * lower bytes; no explicit zero block is needed.
	 */
	if (!changed)
	    continue;

	if (pwrite(out, local_block, local_len, (off_t) offset) !=
	    (ssize_t) local_len) {
	    status = -errno;
	    goto out;
	}
	if (!have_ranges)
	    have_ranges = true;
	{
	    OverlayEntry *r = overlay_entry_find(&data->db, rel);
	    OverlayRange *range;
	    OverlayRange *previous = NULL;
	    OverlayRange **tail;

	    if (r == NULL) {
		status = -ENOMEM;
		goto out;
	    }
	    for (tail = &r->ranges; *tail != NULL; tail = &(*tail)->next)
		previous = *tail;
	    if (previous != NULL
		&& previous->offset + previous->length == offset) {
		previous->length += local_len;
	    } else {
		range = calloc(1, sizeof(*range));
		if (range == NULL) {
		    status = -ENOMEM;
		    goto out;
		}
		range->offset = offset;
		range->length = local_len;
		*tail = range;
	    }
	}
    }

    if (fsync(out) < 0) {
	status = -errno;
	goto out;
    }
    close(out);
    out = -1;
    close(in_local);
    in_local = -1;
    close(in_lower);
    in_lower = -1;

    if (!have_ranges && local_st.st_size == lower_st.st_size) {
	/* The file is byte-identical to the lower copy again.  */
	unlink(temporary);
	unlink(upper);
	overlay_entry_remove(&data->db, rel);
	status = overlay_commit(data);
	if (status < 0)
	    return status;
	return source_unlink(data, rel);
    }

    if (rename(temporary, upper) < 0) {
	status = -errno;
	goto out;
    }

    entry = overlay_entry_get(&data->db, rel, true);
    if (entry == NULL) {
	status = -ENOMEM;
	goto out;
    }
    entry->deleted = false;
    entry->block = true;
    entry->size = (uint64_t) local_st.st_size;
    entry->mtime = local_st.st_mtime;
    entry->mode = lower_st.st_mode;
    entry->block_size = OVERLAY_BLOCK_SIZE;
    {
	char hash[65];

	if (overlay_hash_file(local, hash) == 0)
	    snprintf(entry->hash, sizeof(entry->hash), "%s", hash);
    }
    status = overlay_commit(data);
    if (status < 0)
	return status;
    return source_put(data, rel, upper);

  out:
    if (in_local >= 0)
	close(in_local);
    if (in_lower >= 0)
	close(in_lower);
    if (out >= 0)
	close(out);
    unlink(temporary);
    return status;
}

static int overlay_put(NetfsMount *mount, const char *rel, const char *local)
{
    OverlayData *data = overlay_data(mount);
    int status;
    char lower[OVERLAY_PATH_MAX];
    struct stat st;
    bool lower_exists;

    pthread_mutex_lock(&data->lock);
    if (overlay_private(rel)) {
	pthread_mutex_unlock(&data->lock);
	return -ENOENT;
    }
    if (data->db.mode == OVERLAY_MODE_BLOCK
	&& overlay_join(lower, sizeof(lower), data->db.lower, rel) == 0
	&& lstat(lower, &st) == 0 && S_ISREG(st.st_mode))
	lower_exists = true;
    else
	lower_exists = false;

    {
	char upper[OVERLAY_PATH_MAX];
	struct stat upper_st;

	if (data->db.mode == OVERLAY_MODE_BLOCK && lower_exists
	    && overlay_join(upper, sizeof(upper), data->db.upper, rel) == 0
	    && lstat(upper, &upper_st) == 0 && S_ISREG(upper_st.st_mode)
	    && upper_st.st_nlink > 1)
	    status = store_full(data, rel, local);
	else if (data->db.mode == OVERLAY_MODE_BLOCK && lower_exists)
	    status = store_delta(data, rel, local);
	else
	    status = store_full(data, rel, local);
    }
    pthread_mutex_unlock(&data->lock);
    return status;
}

static int overlay_mkdir(NetfsMount *mount, const char *rel)
{
    OverlayData *data = overlay_data(mount);
    char upper[OVERLAY_PATH_MAX];
    int status;

    pthread_mutex_lock(&data->lock);
    if (overlay_private(rel)) {
	pthread_mutex_unlock(&data->lock);
	return -ENOENT;
    }
    {
	struct stat st;

	if (effective_lstat(data, rel, &st, NULL) == 0) {
	    pthread_mutex_unlock(&data->lock);
	    return -EEXIST;
	}
    }
    if (ensure_parent(data, rel) < 0) {
	pthread_mutex_unlock(&data->lock);
	return -ENOENT;
    }
    if (overlay_join(upper, sizeof(upper), data->db.upper, rel) < 0) {
	pthread_mutex_unlock(&data->lock);
	return -ENAMETOOLONG;
    }
    if (mkdir(upper, 0755) < 0 && errno != EEXIST) {
	status = -errno;
	pthread_mutex_unlock(&data->lock);
	return status;
    }
    overlay_entry_remove(&data->db, rel);
    status = overlay_commit(data);
    if (status == 0)
	status = source_mkdir(data, rel);
    pthread_mutex_unlock(&data->lock);
    return status;
}

static int overlay_rmdir(NetfsMount *mount, const char *rel)
{
    OverlayData *data = overlay_data(mount);
    char upper[OVERLAY_PATH_MAX];
    char lower[OVERLAY_PATH_MAX];
    bool in_lower;
    int status = 0;

    pthread_mutex_lock(&data->lock);
    if (overlay_private(rel)) {
	pthread_mutex_unlock(&data->lock);
	return -ENOENT;
    }
    {
	TALLOC_CTX *ctx = talloc_new(NULL);
	NetfsDirent *entries = NULL;
	size_t count = 0;
	size_t capacity = 0;
	int list_status = 0;

	if (ctx == NULL) {
	    pthread_mutex_unlock(&data->lock);
	    return -ENOMEM;
	}
	list_status = collect_dir(&entries, &count, &capacity, ctx, data, rel,
				  data->db.upper, true);
	if (list_status == 0)
	    list_status = collect_dir(&entries, &count, &capacity, ctx, data,
				      rel, data->db.lower, false);
	talloc_free(ctx);
	if (list_status < 0) {
	    pthread_mutex_unlock(&data->lock);
	    return list_status;
	}
	if (count != 0) {
	    pthread_mutex_unlock(&data->lock);
	    return -ENOTEMPTY;
	}
    }
    if (overlay_join(lower, sizeof(lower), data->db.lower, rel) == 0
	&& lstat(lower, &(struct stat){ 0 }) == 0)
	in_lower = true;
    else
	in_lower = false;
    if (overlay_join(upper, sizeof(upper), data->db.upper, rel) == 0) {
	status = remove_upper_tree(upper);
	if (status < 0) {
	    pthread_mutex_unlock(&data->lock);
	    return status;
	}
    }
    overlay_entry_remove_tree(&data->db, rel);
    if (in_lower) {
	OverlayEntry *entry = overlay_entry_get(&data->db, rel, true);

	if (entry == NULL)
	    status = -ENOMEM;
	else
	    entry->deleted = true;
    }
    if (status == 0)
	status = overlay_commit(data);
    if (status == 0)
	status = source_rmdir(data, rel);
    pthread_mutex_unlock(&data->lock);
    return status;
}

static int overlay_unlink(NetfsMount *mount, const char *rel)
{
    OverlayData *data = overlay_data(mount);
    char upper[OVERLAY_PATH_MAX];
    char lower[OVERLAY_PATH_MAX];
    bool in_lower;
    int status = 0;

    pthread_mutex_lock(&data->lock);
    if (overlay_private(rel)) {
	pthread_mutex_unlock(&data->lock);
	return -ENOENT;
    }
    {
	struct stat st;

	if (effective_lstat(data, rel, &st, NULL) == 0
	    && S_ISDIR(st.st_mode)) {
	    pthread_mutex_unlock(&data->lock);
	    return -EISDIR;
	}
    }
    if (overlay_join(lower, sizeof(lower), data->db.lower, rel) == 0
	&& lstat(lower, &(struct stat){ 0 }) == 0)
	in_lower = true;
    else
	in_lower = false;
    if (overlay_join(upper, sizeof(upper), data->db.upper, rel) == 0)
	(void) remove_upper_tree(upper);
    overlay_entry_remove(&data->db, rel);
    if (in_lower) {
	OverlayEntry *entry = overlay_entry_get(&data->db, rel, true);

	if (entry == NULL)
	    status = -ENOMEM;
	else
	    entry->deleted = true;
    }
    if (status == 0)
	status = overlay_commit(data);
    if (status == 0)
	status = source_unlink(data, rel);
    pthread_mutex_unlock(&data->lock);
    return status;
}

static int copy_effective_tree(OverlayData *data, const char *from,
			       const char *to);

static int copy_effective_file(OverlayData *data, const char *from,
			       const char *to, struct stat *st)
{
    char destination[OVERLAY_PATH_MAX];
    char source[OVERLAY_PATH_MAX];
    int status;

    if (overlay_join(destination, sizeof(destination), data->db.upper, to) < 0)
	return -ENAMETOOLONG;
    if (ensure_parent(data, to) < 0)
	return -ENOENT;
    unlink(destination);
    if (S_ISLNK(st->st_mode)) {
	char target[OVERLAY_PATH_MAX];
	ssize_t got;

	if (overlay_join(source, sizeof(source), data->db.upper, from) < 0)
	    return -ENAMETOOLONG;
	if (lstat(source, &(struct stat){ 0 }) != 0) {
	    if (overlay_join(source, sizeof(source), data->db.lower, from) < 0)
		return -ENAMETOOLONG;
	}
	got = readlink(source, target, sizeof(target) - 1);
	if (got < 0)
	    return -errno;
	target[got] = '\0';
	return symlink(target, destination) < 0 ? -errno : 0;
    }
    if (S_ISDIR(st->st_mode)) {
	if (mkdir(destination, st->st_mode & 07777) < 0 && errno != EEXIST)
	    return -errno;
	(void) chmod(destination, st->st_mode & 07777);
	return 0;
    }

    {
	OverlayEntry *entry = overlay_entry_find(&data->db, from);

	if (entry != NULL && entry->block) {
	    char temporary[OVERLAY_PATH_MAX + 64];

	    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", destination,
			 (long) getpid()) >= (int) sizeof(temporary))
		return -ENAMETOOLONG;
	    unlink(temporary);
	    status = overlay_delta_merge(&data->db, entry, NULL, NULL,
					 temporary);
	    if (status < 0)
		return status;
	    if (rename(temporary, destination) < 0) {
		status = -errno;
		unlink(temporary);
		return status;
	    }
	    return 0;
	}
	if (overlay_join(source, sizeof(source), data->db.upper, from) < 0)
	    return -ENAMETOOLONG;
	if (lstat(source, &(struct stat){ 0 }) != 0) {
	    if (overlay_join(source, sizeof(source), data->db.lower, from) < 0)
		return -ENAMETOOLONG;
	}
	return overlay_copy_file(source, destination, st->st_mode);
    }
}

static int copy_effective_tree(OverlayData *data, const char *from,
			       const char *to)
{
    struct stat st;
    int status;

    status = effective_lstat(data, from, &st, NULL);
    if (status < 0)
	return status;
    status = copy_effective_file(data, from, to, &st);
    if (status < 0 || !S_ISDIR(st.st_mode))
	return status;

    {
	const char *bases[2];
	int pass;

	bases[0] = data->db.upper;
	bases[1] = data->db.lower;
	for (pass = 0; pass < 2; pass++) {
	    char directory[OVERLAY_PATH_MAX];
	    DIR *dir;
	    struct dirent *item;

	    if (overlay_join(directory, sizeof(directory), bases[pass],
			     from) < 0)
		return -ENAMETOOLONG;
	    dir = opendir(directory);
	    if (dir == NULL) {
		if (errno == ENOENT)
		    continue;
		return -errno;
	    }
	    while ((item = readdir(dir)) != NULL) {
		char child_from[OVERLAY_PATH_MAX];
		char child_to[OVERLAY_PATH_MAX];

		if (strcmp(item->d_name, ".") == 0
		    || strcmp(item->d_name, "..") == 0)
		    continue;
		if (overlay_join(child_from, sizeof(child_from), from,
				 item->d_name) < 0
		    || overlay_join(child_to, sizeof(child_to), to,
				    item->d_name) < 0)
		    continue;
		if (overlay_db_hidden(&data->db, child_from))
		    continue;
		if (pass == 1) {
		    char upper_child[OVERLAY_PATH_MAX];

		    if (overlay_join(upper_child, sizeof(upper_child),
				     data->db.upper, child_from) == 0
			&& lstat(upper_child, &(struct stat){ 0 }) == 0)
			continue;
		}
		status = copy_effective_tree(data, child_from, child_to);
		if (status < 0)
		    break;
	    }
	    closedir(dir);
	    if (status < 0)
		break;
	}
    }
    return status;
}


static int source_delete_tree(OverlayData *data, const char *rel)
{
    struct stat st;
    int status;

    if (data->upper_ops == NULL || data->upper_ops->stat == NULL)
	return -ENOSYS;
    status = data->upper_ops->stat(data->upper_source, rel, &st);
    if (status == -ENOENT)
	return 0;
    if (status < 0)
	return status;
    if (S_ISDIR(st.st_mode)) {
	TALLOC_CTX *ctx = talloc_new(NULL);
	NetfsDirent *entries = NULL;
	size_t count = 0;
	size_t i;

	if (ctx == NULL)
	    return -ENOMEM;
	status = data->upper_ops->list(data->upper_source, rel, &entries,
				       &count, ctx);
	if (status < 0) {
	    talloc_free(ctx);
	    return status;
	}
	for (i = 0; i < count; i++) {
	    char child[OVERLAY_PATH_MAX];

	    if (strcmp(entries[i].name, ".") == 0
		|| strcmp(entries[i].name, "..") == 0)
		continue;
	    if (rel[0] == '\0') {
		if (snprintf(child, sizeof(child), "%s", entries[i].name)
		    >= (int) sizeof(child)) {
		    talloc_free(ctx);
		    return -ENAMETOOLONG;
		}
	    } else if (snprintf(child, sizeof(child), "%s/%s", rel,
				entries[i].name) >= (int) sizeof(child)) {
		talloc_free(ctx);
		return -ENAMETOOLONG;
	    }
	    status = source_delete_tree(data, child);
	    if (status < 0) {
		talloc_free(ctx);
		return status;
	    }
	}
	talloc_free(ctx);
	return data->upper_ops->rmdir(data->upper_source, rel);
    }
    return data->upper_ops->unlink(data->upper_source, rel);
}

static int source_put_tree(OverlayData *data, const char *rel)
{
    char local[OVERLAY_PATH_MAX];
    struct stat st;
    int status;

    if (overlay_join(local, sizeof(local), data->db.upper, rel) < 0)
	return -ENAMETOOLONG;
    if (lstat(local, &st) < 0)
	return errno == ENOENT ? 0 : -errno;
    if (S_ISDIR(st.st_mode)) {
	DIR *dir;
	struct dirent *item;

	if (rel[0] != '\0') {
	    status = source_mkdir(data, rel);
	    if (status < 0)
		return status;
	}
	dir = opendir(local);
	if (dir == NULL)
	    return -errno;
	while ((item = readdir(dir)) != NULL) {
	    char child[OVERLAY_PATH_MAX];

	    if (strcmp(item->d_name, ".") == 0
		|| strcmp(item->d_name, "..") == 0)
		continue;
	    if (rel[0] == '\0') {
		if (snprintf(child, sizeof(child), "%s", item->d_name)
		    >= (int) sizeof(child)) {
		    closedir(dir);
		    return -ENAMETOOLONG;
		}
	    } else if (snprintf(child, sizeof(child), "%s/%s", rel,
				item->d_name) >= (int) sizeof(child)) {
		closedir(dir);
		return -ENAMETOOLONG;
	    }
	    status = source_put_tree(data, child);
	    if (status < 0) {
		closedir(dir);
		return status;
	    }
	}
	closedir(dir);
	return 0;
    }
    if (S_ISLNK(st.st_mode)) {
	char target[OVERLAY_PATH_MAX];
	ssize_t got = readlink(local, target, sizeof(target) - 1);

	if (got < 0)
	    return -errno;
	target[got] = '\0';
	return source_symlink(data, rel, target);
    }
    return source_put(data, rel, local);
}

static int sync_upper_all(OverlayData *data)
{
    TALLOC_CTX *ctx;
    NetfsDirent *entries = NULL;
    size_t count = 0;
    size_t i;
    int status;

    if (data->upper_source == NULL)
	return 0;
    if (data->upper_ops == NULL || data->upper_ops->list == NULL)
	return -ENOSYS;
    ctx = talloc_new(NULL);
    if (ctx == NULL)
	return -ENOMEM;
    status = data->upper_ops->list(data->upper_source, "", &entries, &count,
				   ctx);
    if (status < 0) {
	talloc_free(ctx);
	return status;
    }
    for (i = 0; i < count; i++) {
	char child[OVERLAY_PATH_MAX];

	if (strcmp(entries[i].name, ".") == 0
	    || strcmp(entries[i].name, "..") == 0)
	    continue;
	if (snprintf(child, sizeof(child), "%s", entries[i].name)
	    >= (int) sizeof(child)) {
	    talloc_free(ctx);
	    return -ENAMETOOLONG;
	}
	status = source_delete_tree(data, child);
	if (status < 0) {
	    talloc_free(ctx);
	    return status;
	}
    }
    talloc_free(ctx);
    return source_put_tree(data, "");
}


/*
 * Atomic editors write a temporary file and rename it over the target.
 * Treating that rename as a full copy would silently turn a block-mode
 * upper into a whole-file copy and drop the delta entry.  When the
 * destination exists in the lower layer, build the destination delta
 * from the source content instead.
 */
static int rename_as_delta(OverlayData *data, const char *from,
			   const char *to, const char *upper_from,
			   const char *lower_from, bool source_in_lower)
{
    char lower_to[OVERLAY_PATH_MAX];
    char upper_to[OVERLAY_PATH_MAX];
    char temporary[OVERLAY_PATH_MAX + 64];
    struct stat source_st;
    struct stat lower_st;
    OverlayEntry *entry;
    int status;

    if (overlay_join(lower_to, sizeof(lower_to), data->db.lower, to) < 0
	|| overlay_join(upper_to, sizeof(upper_to), data->db.upper, to) < 0)
	return -ENAMETOOLONG;
    if (lstat(lower_to, &lower_st) < 0 || !S_ISREG(lower_st.st_mode))
	return -ENOTSUP;
    status = effective_lstat(data, from, &source_st, NULL);
    if (status < 0)
	return status;
    if (!S_ISREG(source_st.st_mode))
	return -ENOTSUP;

    if (snprintf(temporary, sizeof(temporary), "/tmp/.uvroot-rename.%ld",
		 (long) getpid()) >= (int) sizeof(temporary))
	return -ENAMETOOLONG;
    unlink(temporary);

    entry = overlay_entry_find(&data->db, from);
    if (entry != NULL && entry->block) {
	status = overlay_delta_merge(&data->db, entry, NULL, NULL, temporary);
    } else if (lstat(upper_from, &(struct stat){ 0 }) == 0) {
	status = overlay_copy_file(upper_from, temporary, source_st.st_mode);
    } else {
	status = overlay_copy_file(lower_from, temporary, source_st.st_mode);
    }
    if (status < 0) {
	unlink(temporary);
	return status;
    }

    (void) remove_upper_tree(upper_from);
    overlay_entry_remove_tree(&data->db, from);
    if (source_in_lower) {
	OverlayEntry *whiteout = overlay_entry_get(&data->db, from, true);

	if (whiteout != NULL)
	    whiteout->deleted = true;
    }

    (void) remove_upper_tree(upper_to);
    overlay_entry_remove_tree(&data->db, to);
    status = store_delta(data, to, temporary);
    unlink(temporary);
    if (status == 0) {
	if (data->upper_source != NULL)
	    status = source_unlink(data, from);
	if (status == 0)
	    status = overlay_commit(data);
    }
    return status;
}

static int overlay_rename(NetfsMount *mount, const char *from, const char *to)
{
    OverlayData *data = overlay_data(mount);
    char upper_from[OVERLAY_PATH_MAX];
    char upper_to[OVERLAY_PATH_MAX];
    char lower_from[OVERLAY_PATH_MAX];
    bool source_in_lower;
    int status;

    pthread_mutex_lock(&data->lock);
    if (strcmp(from, to) == 0) {
	pthread_mutex_unlock(&data->lock);
	return 0;
    }
    if (overlay_private(from) || overlay_private(to)
	|| overlay_db_hidden(&data->db, from)
	|| overlay_db_hidden(&data->db, to)) {
	pthread_mutex_unlock(&data->lock);
	return -ENOENT;
    }
    {
	size_t from_length = strlen(from);

	if (from_length < OVERLAY_PATH_MAX
	    && strncmp(to, from, from_length) == 0
	    && to[from_length] == '/') {
	    pthread_mutex_unlock(&data->lock);
	    return -EINVAL;
	}
    }
    if (overlay_join(upper_from, sizeof(upper_from), data->db.upper, from) < 0
	|| overlay_join(upper_to, sizeof(upper_to), data->db.upper, to) < 0
	|| overlay_join(lower_from, sizeof(lower_from), data->db.lower,
			from) < 0) {
	pthread_mutex_unlock(&data->lock);
	return -ENAMETOOLONG;
    }
    source_in_lower = lstat(lower_from, &(struct stat){ 0 }) == 0;

    if (data->db.mode == OVERLAY_MODE_BLOCK) {
	status = rename_as_delta(data, from, to, upper_from, lower_from,
				 source_in_lower);
	if (status != -ENOTSUP) {
	    pthread_mutex_unlock(&data->lock);
	    return status;
	}
    }

    (void) remove_upper_tree(upper_to);
    overlay_entry_remove_tree(&data->db, to);
    status = copy_effective_tree(data, from, to);
    if (status < 0) {
	pthread_mutex_unlock(&data->lock);
	return status;
    }
    (void) remove_upper_tree(upper_from);
    overlay_entry_remove_tree(&data->db, from);
    if (source_in_lower) {
	OverlayEntry *entry = overlay_entry_get(&data->db, from, true);

	if (entry != NULL)
	    entry->deleted = true;
    }
    status = overlay_commit(data);
    if (status == 0 && data->upper_source != NULL)
	status = sync_upper_all(data);
    pthread_mutex_unlock(&data->lock);
    return status;
}

static int overlay_symlink(NetfsMount *mount, const char *rel,
			   const char *target)
{
    OverlayData *data = overlay_data(mount);
    char upper[OVERLAY_PATH_MAX];
    int status;

    pthread_mutex_lock(&data->lock);
    if (overlay_private(rel)) {
	pthread_mutex_unlock(&data->lock);
	return -ENOENT;
    }
    {
	struct stat st;

	if (effective_lstat(data, rel, &st, NULL) == 0) {
	    pthread_mutex_unlock(&data->lock);
	    return -EEXIST;
	}
    }
    if (ensure_parent(data, rel) < 0) {
	pthread_mutex_unlock(&data->lock);
	return -ENOENT;
    }
    if (overlay_join(upper, sizeof(upper), data->db.upper, rel) < 0) {
	pthread_mutex_unlock(&data->lock);
	return -ENAMETOOLONG;
    }
    if (symlink(target, upper) < 0) {
	status = -errno;
	pthread_mutex_unlock(&data->lock);
	return status;
    }
    overlay_entry_remove(&data->db, rel);
    status = overlay_commit(data);
    if (status == 0)
	status = source_symlink(data, rel, target);
    pthread_mutex_unlock(&data->lock);
    return status;
}


static int overlay_link(NetfsMount *mount, const char *from, const char *to)
{
    OverlayData *data = overlay_data(mount);
    char upper_from[OVERLAY_PATH_MAX];
    char upper_to[OVERLAY_PATH_MAX];
    char lower_from[OVERLAY_PATH_MAX];
    struct stat st;
    OverlayEntry *entry;
    int status = 0;

    pthread_mutex_lock(&data->lock);
    if (overlay_private(from) || overlay_private(to)
	|| overlay_db_hidden(&data->db, from)
	|| overlay_db_hidden(&data->db, to)) {
	pthread_mutex_unlock(&data->lock);
	return -ENOENT;
    }
    status = effective_lstat(data, from, &st, NULL);
    if (status < 0)
	goto out;
    if (S_ISDIR(st.st_mode)) {
	status = -EPERM;
	goto out;
    }
    if (effective_lstat(data, to, &(struct stat){ 0 }, NULL) == 0) {
	status = -EEXIST;
	goto out;
    }
    if (overlay_join(upper_from, sizeof(upper_from), data->db.upper, from) < 0
	|| overlay_join(upper_to, sizeof(upper_to), data->db.upper, to) < 0
	|| overlay_join(lower_from, sizeof(lower_from), data->db.lower, from) < 0) {
	status = -ENAMETOOLONG;
	goto out;
    }
    if (ensure_parent(data, to) < 0) {
	status = -ENOENT;
	goto out;
    }

    entry = overlay_entry_find(&data->db, from);
    if (entry != NULL && entry->block) {
	status = overlay_delta_merge(&data->db, entry, NULL, NULL, upper_from);
	if (status < 0)
	    goto out;
	overlay_entry_remove(&data->db, from);
    } else if (lstat(upper_from, &(struct stat){ 0 }) != 0) {
	status = overlay_copy_file(lower_from, upper_from, st.st_mode);
	if (status < 0)
	    goto out;
    }

    (void) remove_upper_tree(upper_to);
    overlay_entry_remove_tree(&data->db, to);
    if (link(upper_from, upper_to) < 0) {
	status = -errno;
	goto out;
    }
    status = overlay_commit(data);
    if (status == 0) {
	if (data->upper_source != NULL)
	    status = sync_upper_all(data);
	else
	    status = source_link(data, from, to);
    }

  out:
    pthread_mutex_unlock(&data->lock);
    return status;
}

static int overlay_statfs(NetfsMount *mount, const char *rel UNUSED,
			  struct statfs *st)
{
    OverlayData *data = overlay_data(mount);

    return statfs(data->db.upper, st) < 0 ? -errno : 0;
}

static int overlay_init(NetfsMount *mount, const char *mode,
			const char *upper, const char *lower,
			const char *guest, NetfsMount *upper_source,
			const char *db_override)
{
    OverlayData *data;
    char db_path[OVERLAY_PATH_MAX];
    int status;

    data = talloc_zero(mount, OverlayData);
    if (data == NULL)
	return -ENOMEM;
    pthread_mutex_init(&data->lock, NULL);
    mount->backend_data = data;
    data->db.mode = strcmp(mode, "block") == 0
	? OVERLAY_MODE_BLOCK : OVERLAY_MODE_BIND;
    if (db_override != NULL)
	snprintf(db_path, sizeof(db_path), "%s", db_override);
    else
	snprintf(db_path, sizeof(db_path), "%s/%s", upper, OVERLAY_DB_NAME);
    status = overlay_db_load(&data->db, db_path);
    if (status < 0) {
	talloc_free(data);
	mount->backend_data = NULL;
	return status;
    }
    /*
     * The mode selects the copy-up policy, not the on-disk format of
     * existing entries.  Each entry records whether it is a full file or
     * a sparse block delta, so a tree created in one mode is readable in
     * the other; the selected mode is saved for subsequent writes.
     */
    free(data->db.upper);
    free(data->db.lower);
    free(data->db.guest);
    data->db.upper = strdup(upper);
    data->db.lower = strdup(lower);
    data->db.guest = strdup(guest);
    if (data->db.upper == NULL || data->db.lower == NULL
	|| data->db.guest == NULL)
	return -ENOMEM;
    data->db.mode = strcmp(mode, "block") == 0
	? OVERLAY_MODE_BLOCK : OVERLAY_MODE_BIND;
    data->db.read_only = mount->read_only;
    data->upper_source = upper_source;
    data->upper_ops = upper_source != NULL
	? netfs_dir_ops_of(upper_source) : NULL;
    data->db.upper_source = upper_source != NULL;
    return overlay_commit(data);
}

const char *netfs_overlay_lower(NetfsMount *mount)
{
    OverlayData *data = overlay_data(mount);

    return data != NULL ? data->db.lower : NULL;
}

const char *netfs_overlay_upper(NetfsMount *mount)
{
    OverlayData *data = overlay_data(mount);

    return data != NULL ? data->db.upper : NULL;
}

void netfs_overlay_set_read_only(NetfsMount *mount)
{
    OverlayData *data = overlay_data(mount);

    if (data == NULL)
	return;
    pthread_mutex_lock(&data->lock);
    data->db.read_only = true;
    (void) overlay_commit(data);
    pthread_mutex_unlock(&data->lock);
}

static void overlay_fini(NetfsMount *mount)
{
    OverlayData *data = overlay_data(mount);

    if (data == NULL)
	return;
    pthread_mutex_destroy(&data->lock);
    overlay_db_free(&data->db);
}

const NetfsBackend netfs_backend_overlay = {
    .name = "overlay",
    .schemes = NULL,
    .kind = NETFS_KIND_DIR,
    .implemented = true,
    .reserved_note = NULL,
    .dir = {
	.list = overlay_list,
	.stat = overlay_stat,
	.get = overlay_get,
	.put = overlay_put,
	.mkdir = overlay_mkdir,
	.rmdir = overlay_rmdir,
	.unlink = overlay_unlink,
	.rename = overlay_rename,
	.symlink = overlay_symlink,
	.link = overlay_link,
	.chmod = NULL,
	.chown = NULL,
	.statfs = overlay_statfs,
    },
    .block = { 0 },
    .init = NULL,
    .fini = overlay_fini,
};

int netfs_overlay_init(NetfsMount *mount, const char *mode, const char *upper,
		       const char *lower, const char *guest,
		       NetfsMount *upper_source, const char *db_override)
{
    return overlay_init(mount, mode, upper, lower, guest, upper_source,
			db_override);
}
