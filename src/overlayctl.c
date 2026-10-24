/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * bkc / bme / brd: inspect, merge and deduplicate overlay block deltas.
 *
 * This is a small static helper bound to /dev/bkc, /dev/bme and /dev/brd.
 * It
 * runs inside the container, so geteuid() and stat(2) go through vperm
 * when --vperm is enabled; that is how the merge permission check sees
 * the virtual identity and the virtual file owner.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "extension/overlay/overlay_db.h"

#define UPPER_VIEW "/.uvroot-overlay/upper"
#define LOWER_VIEW "/.uvroot-overlay/lower"
#define DB_VIEW UPPER_VIEW "/" OVERLAY_DB_NAME

static char *host_upper;
static char *host_lower;

static int save_helper_db(OverlayDb *db)
{
    char *upper = db->upper;
    char *lower = db->lower;
    int status;

    db->upper = host_upper;
    db->lower = host_lower;
    status = overlay_db_save(db);
    db->upper = upper;
    db->lower = lower;
    return status;
}

static void usage(const char *name)
{
    fprintf(stderr,
	    "usage: %s [-r] PATH\n"
	    "       %s [-h] [-r] PATH\n"
	    "       brd [-r] PATH\n", name, name);
}

static int normalize(const char *input, char *output, size_t size)
{
    char buffer[OVERLAY_PATH_MAX];
    char *stack[OVERLAY_PATH_MAX / 2];
    size_t depth = 0;
    char *cursor;
    size_t used = 0;

    if (input[0] == '/') {
	if (snprintf(buffer, sizeof(buffer), "%s", input) >= (int) sizeof(buffer))
	    return -ENAMETOOLONG;
    } else {
	char cwd[OVERLAY_PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
	    return -errno;
	if (snprintf(buffer, sizeof(buffer), "%s/%s", cwd, input)
	    >= (int) sizeof(buffer))
	    return -ENAMETOOLONG;
    }

    cursor = buffer;
    while (*cursor != '\0') {
	char *start;

	while (*cursor == '/')
	    cursor++;
	if (*cursor == '\0')
	    break;
	start = cursor;
	while (*cursor != '\0' && *cursor != '/')
	    cursor++;
	if (*cursor != '\0')
	    *cursor++ = '\0';
	if (strcmp(start, ".") == 0)
	    continue;
	if (strcmp(start, "..") == 0) {
	    if (depth > 0)
		depth--;
	    continue;
	}
	if (depth < sizeof(stack) / sizeof(stack[0]))
	    stack[depth++] = start;
    }

    output[0] = '\0';
    for (size_t i = 0; i < depth; i++) {
	int written = snprintf(output + used, size - used, "/%s", stack[i]);

	if (written < 0 || (size_t) written >= size - used)
	    return -ENAMETOOLONG;
	used += (size_t) written;
    }
    if (used == 0) {
	if (size < 2)
	    return -ENAMETOOLONG;
	output[0] = '/';
	output[1] = '\0';
    }
    return 0;
}

static int rel_from_guest(OverlayDb *db, const char *path, char *rel,
			  size_t size)
{
    char normalized[OVERLAY_PATH_MAX];
    size_t guest_length;

    if (normalize(path, normalized, sizeof(normalized)) < 0)
	return -EINVAL;
    if (db->guest == NULL || db->guest[0] != '/')
	return -EINVAL;
    guest_length = strlen(db->guest);
    while (guest_length > 1 && db->guest[guest_length - 1] == '/')
	guest_length--;
    if (strncmp(normalized, db->guest, guest_length) != 0)
	return -EINVAL;
    if (normalized[guest_length] == '\0') {
	rel[0] = '\0';
	return 0;
    }
    if (normalized[guest_length] != '/')
	return -EINVAL;
    if (strlen(normalized + guest_length + 1) >= size)
	return -ENAMETOOLONG;
    snprintf(rel, size, "%s", normalized + guest_length + 1);
    return 0;
}

static bool merge_allowed(OverlayDb *db, const char *rel)
{
    char path[OVERLAY_PATH_MAX];
    struct stat st;
    uid_t uid = geteuid();

    if (overlay_join(path, sizeof(path), db->guest, rel) < 0)
	return false;
    if (uid == 0)
	return true;
    if (stat(path, &st) < 0)
	return false;
    return st.st_uid == uid;
}

static int merge_entry(OverlayDb *db, OverlayEntry *entry, bool verify,
		       const char *rel)
{
    char destination[OVERLAY_PATH_MAX];
    char guest_path[OVERLAY_PATH_MAX];
    char hash[65];
    int status;

    if (db->read_only) {
	fprintf(stderr, "bme: %s: read-only overlay\n", rel);
	return -EROFS;
    }
    if (db->upper_source) {
	fprintf(stderr,
		"bme: %s: type 1 is a source; run bme on a directory-backed "
		"type 1\n", rel);
	return -EOPNOTSUPP;
    }
    if (!merge_allowed(db, rel)) {
	fprintf(stderr, "bme: %s: permission denied\n", rel);
	return -EPERM;
    }
    if (overlay_join(destination, sizeof(destination), db->upper, rel) < 0
	|| overlay_join(guest_path, sizeof(guest_path), db->guest, rel) < 0)
	return -ENAMETOOLONG;

    if (verify) {
	char temporary[OVERLAY_PATH_MAX + 64];

	snprintf(temporary, sizeof(temporary), "%s.verify", destination);
	unlink(temporary);
	status = overlay_delta_merge(db, entry, NULL, NULL, temporary);
	if (status < 0) {
	    unlink(temporary);
	    return status;
	}
	(void) chmod(temporary, S_IRUSR | S_IWUSR);
	status = overlay_hash_file(temporary, hash);
	unlink(temporary);
	if (status < 0)
	    return status;
	if (strcmp(hash, entry->hash) != 0) {
	    fprintf(stderr, "bme: %s: hash mismatch\n", rel);
	    return -EIO;
	}
    }

    status = overlay_delta_merge(db, entry, NULL, NULL, destination);
    if (status < 0) {
	fprintf(stderr, "bme: %s: %s\n", rel, strerror(-status));
	return status;
    }
    overlay_entry_remove(db, rel);
    (void) guest_path;
    printf("merged %s\n", rel);
    return 0;
}

static int check_entry(OverlayDb *db, OverlayEntry *entry, const char *rel)
{
    char temporary[OVERLAY_PATH_MAX + 64];
    char hash[65];
    int status;

    if (entry != NULL && entry->deleted) {
	printf("%s deleted\n", rel);
	return 1;
    }
    if (entry == NULL || !entry->block) {
	/* No block delta: the upper file, when present, is the whole file.  */
	char upper[OVERLAY_PATH_MAX];
	char lower[OVERLAY_PATH_MAX];
	char upper_hash[65];
	char lower_hash[65];
	struct stat st;

	if (overlay_join(upper, sizeof(upper), db->upper, rel) < 0
	    || overlay_join(lower, sizeof(lower), db->lower, rel) < 0)
	    return -ENAMETOOLONG;
	if (lstat(upper, &st) == 0)
	    status = overlay_hash_file(upper, upper_hash);
	else
	    status = -ENOENT;
	if (status < 0)
	    upper_hash[0] = '\0';
	status = overlay_hash_file(lower, lower_hash);
	if (status < 0)
	    lower_hash[0] = '\0';
	printf("%s merged", rel);
	if (upper_hash[0] != '\0' && lower_hash[0] != '\0') {
	    if (strcmp(upper_hash, lower_hash) == 0)
		printf(" hash-ok");
	    else
		printf(" hash-diff");
	}
	printf("\n");
	return 0;
    }

    snprintf(temporary, sizeof(temporary), "/tmp/.bkc-check.%ld",
		 (long) getpid());
    unlink(temporary);
    status = overlay_delta_merge(db, entry, NULL, NULL, temporary);
    if (status < 0) {
	unlink(temporary);
	fprintf(stderr, "bkc: %s: %s\n", rel, strerror(-status));
	return status;
    }
    (void) chmod(temporary, S_IRUSR | S_IWUSR);
    status = overlay_hash_file(temporary, hash);
    unlink(temporary);
    if (status < 0)
	return status;

    printf("%s unmerged", rel);
    if (strcmp(hash, entry->hash) == 0)
	printf(" hash-ok\n");
    else
	printf(" hash-mismatch\n");
    return strcmp(hash, entry->hash) == 0 ? 1 : 2;
}


static void read_block(int fd, unsigned char *buffer, size_t length, off_t offset)
{
    size_t done = 0;

    memset(buffer, 0, length);
    if (fd < 0)
	return;
    while (done < length) {
	ssize_t got = pread(fd, buffer + done, length - done, offset + done);

	if (got <= 0)
	    break;
	done += (size_t) got;
    }
}

static int write_block(int fd, const unsigned char *buffer, size_t length,
		       off_t offset)
{
    size_t done = 0;

    while (done < length) {
	ssize_t written = pwrite(fd, buffer + done, length - done,
				 offset + done);

	if (written <= 0)
	    return -errno;
	done += (size_t) written;
    }
    return 0;
}

static int add_range(OverlayRange **head, OverlayRange **tail, uint64_t offset,
		     uint64_t length)
{
    OverlayRange *range;

    if (*tail != NULL && (*tail)->offset + (*tail)->length == offset) {
	(*tail)->length += length;
	return 0;
    }
    range = calloc(1, sizeof(*range));
    if (range == NULL)
	return -ENOMEM;
    range->offset = offset;
    range->length = length;
    if (*tail != NULL)
	(*tail)->next = range;
    else
	*head = range;
    *tail = range;
    return 0;
}

/*
 * Remove 4 KiB chunks from a block entry when they are byte-identical to
 * the corresponding lower chunk.  The stored hash describes the merged
 * result and therefore does not change.
 */
static int dedupe_block(OverlayDb *db, OverlayEntry *entry, const char *rel,
			bool *changed)
{
    char upper[OVERLAY_PATH_MAX];
    char lower[OVERLAY_PATH_MAX];
    char temporary[OVERLAY_PATH_MAX + 64];
    struct stat lower_st;
    struct stat upper_st;
    OverlayRange *range;
    OverlayRange *new_ranges = NULL;
    OverlayRange *new_tail = NULL;
    int upper_fd = -1;
    int lower_fd = -1;
    int temp_fd = -1;
    int status = 0;
    bool upper_exists;
    bool dropped = false;
    uint64_t max_end = 0;

    if (!merge_allowed(db, rel)) {
	fprintf(stderr, "brd: %s: permission denied\n", rel);
	return -EPERM;
    }
    if (overlay_join(upper, sizeof(upper), db->upper, rel) < 0
	|| overlay_join(lower, sizeof(lower), db->lower, rel) < 0)
	return -ENAMETOOLONG;
    if (lstat(lower, &lower_st) < 0 || !S_ISREG(lower_st.st_mode)) {
	printf("kept %s\n", rel);
	return 0;
    }
    upper_exists = lstat(upper, &upper_st) == 0
	&& S_ISREG(upper_st.st_mode);

    if (entry->ranges == NULL) {
	if (entry->size == (uint64_t) lower_st.st_size) {
	    if (upper_exists)
		(void) unlink(upper);
	    overlay_entry_remove(db, rel);
	    printf("deduped %s\n", rel);
	    *changed = true;
	} else {
	    printf("kept %s\n", rel);
	}
	return 0;
    }
    if (!upper_exists) {
	printf("kept %s\n", rel);
	return 0;
    }

    if (snprintf(temporary, sizeof(temporary), "/tmp/.uvroot-brd.%ld",
		 (long) getpid()) >= (int) sizeof(temporary))
	return -ENAMETOOLONG;
    unlink(temporary);
    temp_fd = open(temporary, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (temp_fd < 0)
	return -errno;
    upper_fd = open(upper, O_RDONLY);
    lower_fd = open(lower, O_RDONLY);

    for (range = entry->ranges; range != NULL; range = range->next) {
	uint64_t done = 0;

	while (done < range->length) {
	    unsigned char upper_block[OVERLAY_BLOCK_SIZE];
	    unsigned char lower_block[OVERLAY_BLOCK_SIZE];
	    size_t chunk = range->length - done;

	    if (chunk > OVERLAY_BLOCK_SIZE)
		chunk = OVERLAY_BLOCK_SIZE;
	    read_block(upper_fd, upper_block, chunk, range->offset + done);
	    read_block(lower_fd, lower_block, chunk, range->offset + done);
	    if (memcmp(upper_block, lower_block, chunk) != 0) {
		status = write_block(temp_fd, upper_block, chunk,
				     range->offset + done);
		if (status < 0)
		    goto out;
		status = add_range(&new_ranges, &new_tail,
				   range->offset + done, chunk);
		if (status < 0)
		    goto out;
		if (range->offset + done + chunk > max_end)
		    max_end = range->offset + done + chunk;
	    } else {
		dropped = true;
	    }
	    done += chunk;
	}
    }

    if (!dropped) {
	printf("kept %s\n", rel);
	goto out;
    }

    if (new_ranges == NULL) {
	(void) unlink(upper);
	if (entry->size == (uint64_t) lower_st.st_size) {
	    overlay_entry_remove(db, rel);
	} else {
	    OverlayRange *old = entry->ranges;

	    entry->ranges = NULL;
	    while (old != NULL) {
		OverlayRange *next = old->next;

		free(old);
		old = next;
	    }
	}
	printf("deduped %s\n", rel);
	*changed = true;
	goto out;
    }

    if (ftruncate(temp_fd, (off_t) max_end) < 0) {
	status = -errno;
	goto out;
    }
    (void) fchmod(temp_fd, upper_st.st_mode & 07777);
    close(upper_fd);
    upper_fd = -1;
    close(lower_fd);
    lower_fd = -1;
    close(temp_fd);
    temp_fd = -1;
    if (rename(temporary, upper) < 0) {
	status = -errno;
	goto out;
    }
    {
	OverlayRange *old = entry->ranges;

	entry->ranges = new_ranges;
	new_ranges = NULL;
	while (old != NULL) {
	    OverlayRange *next = old->next;

	    free(old);
	    old = next;
	}
    }
    printf("deduped %s\n", rel);
    *changed = true;

  out:
    if (upper_fd >= 0)
	close(upper_fd);
    if (lower_fd >= 0)
	close(lower_fd);
    if (temp_fd >= 0)
	close(temp_fd);
    if (status < 0)
	unlink(temporary);
    while (new_ranges != NULL) {
	OverlayRange *next = new_ranges->next;

	free(new_ranges);
	new_ranges = next;
    }
    return status;
}

static int dedupe_full(OverlayDb *db, const char *rel, bool *changed)
{
    char upper[OVERLAY_PATH_MAX];
    char lower[OVERLAY_PATH_MAX];
    struct stat upper_st;
    struct stat lower_st;
    char upper_hash[65];
    char lower_hash[65];
    int status;

    if (!merge_allowed(db, rel)) {
	fprintf(stderr, "brd: %s: permission denied\n", rel);
	return -EPERM;
    }
    if (overlay_join(upper, sizeof(upper), db->upper, rel) < 0
	|| overlay_join(lower, sizeof(lower), db->lower, rel) < 0)
	return -ENAMETOOLONG;
    if (lstat(upper, &upper_st) < 0 || lstat(lower, &lower_st) < 0) {
	printf("kept %s\n", rel);
	return 0;
    }

    if (S_ISLNK(upper_st.st_mode) && S_ISLNK(lower_st.st_mode)) {
	char upper_target[OVERLAY_PATH_MAX];
	char lower_target[OVERLAY_PATH_MAX];
	ssize_t upper_length;
	ssize_t lower_length;

	upper_length = readlink(upper, upper_target, sizeof(upper_target) - 1);
	lower_length = readlink(lower, lower_target, sizeof(lower_target) - 1);
	if (upper_length < 0 || lower_length < 0) {
	    printf("kept %s\n", rel);
	    return 0;
	}
	upper_target[upper_length] = '\0';
	lower_target[lower_length] = '\0';
	if (strcmp(upper_target, lower_target) == 0) {
	    (void) unlink(upper);
	    overlay_entry_remove(db, rel);
	    printf("deduped %s\n", rel);
	    *changed = true;
	    return 0;
	}
	printf("kept %s\n", rel);
	return 0;
    }

    if (!S_ISREG(upper_st.st_mode) || !S_ISREG(lower_st.st_mode)
	|| upper_st.st_size != lower_st.st_size
	|| (upper_st.st_mode & 07777) != (lower_st.st_mode & 07777)) {
	printf("kept %s\n", rel);
	return 0;
    }
    status = overlay_hash_file(upper, upper_hash);
    if (status < 0)
	return status;
    status = overlay_hash_file(lower, lower_hash);
    if (status < 0)
	return status;
    if (strcmp(upper_hash, lower_hash) != 0) {
	printf("kept %s\n", rel);
	return 0;
    }

    (void) unlink(upper);
    overlay_entry_remove(db, rel);
    printf("deduped %s\n", rel);
    *changed = true;
    return 0;
}


static bool brd_private(const char *rel)
{
    const char *base = strrchr(rel, '/');

    base = base != NULL ? base + 1 : rel;
    if (strncmp(base, OVERLAY_DB_NAME, strlen(OVERLAY_DB_NAME)) == 0) {
	char next = base[strlen(OVERLAY_DB_NAME)];

	return next == '\0' || next == '.';
    }
    return strncmp(base, ".bkc-check", sizeof(".bkc-check") - 1) == 0
	|| strncmp(base, ".bme-verify", sizeof(".bme-verify") - 1) == 0;
}

static int dedupe_path(OverlayDb *db, const char *rel, bool *changed)
{
    OverlayEntry *entry = overlay_entry_find(db, rel);

    if (entry != NULL && entry->deleted) {
	printf("deleted %s\n", rel);
	return 0;
    }
    if (entry != NULL && entry->block)
	return dedupe_block(db, entry, rel, changed);
    return dedupe_full(db, rel, changed);
}

static int dedupe_tree(OverlayDb *db, const char *rel, bool *changed,
		       int *result)
{
    char directory[OVERLAY_PATH_MAX];
    DIR *dir;
    struct dirent *item;

    if (overlay_join(directory, sizeof(directory), db->upper, rel) < 0)
	return -ENAMETOOLONG;
    dir = opendir(directory);
    if (dir == NULL) {
	if (errno == ENOENT || errno == ENOTDIR)
	    return 0;
	return -errno;
    }
    while ((item = readdir(dir)) != NULL) {
	char child[OVERLAY_PATH_MAX];
	char path[OVERLAY_PATH_MAX];
	struct stat st;
	int status;

	if (strcmp(item->d_name, ".") == 0
	    || strcmp(item->d_name, "..") == 0
	    || brd_private(item->d_name))
	    continue;
	if (rel[0] == '\0') {
	    if (snprintf(child, sizeof(child), "%s", item->d_name)
		>= (int) sizeof(child)) {
		closedir(dir);
		return -ENAMETOOLONG;
	    }
	} else if (snprintf(child, sizeof(child), "%s/%s", rel, item->d_name)
		   >= (int) sizeof(child)) {
	    closedir(dir);
	    return -ENAMETOOLONG;
	}
	if (overlay_db_hidden(db, child))
	    continue;
	if (overlay_join(path, sizeof(path), db->upper, child) < 0
	    || lstat(path, &st) < 0)
	    continue;
	if (S_ISDIR(st.st_mode)) {
	    status = dedupe_tree(db, child, changed, result);
	    if (status < 0)
		*result = 1;
	    continue;
	}
	{
	    OverlayEntry *entry = overlay_entry_find(db, child);

	    if (entry != NULL && (entry->block || entry->deleted))
		continue;
	}
	status = dedupe_full(db, child, changed);
	if (status < 0) {
	    *result = 1;
	    continue;
	}
    }
    closedir(dir);
    return 0;
}

static int dedupe_walk(OverlayDb *db, const char *prefix, bool *changed)
{
    OverlayEntry *entry;
    size_t prefix_length = strlen(prefix);
    int result = 0;

    for (entry = db->entries; entry != NULL; ) {
	OverlayEntry *next = entry->next;
	bool matches = strcmp(entry->path, prefix) == 0
	    || prefix_length == 0
	    || (strncmp(entry->path, prefix, prefix_length) == 0
		&& entry->path[prefix_length] == '/');

	if (matches && !entry->deleted && entry->block
	    && dedupe_block(db, entry, entry->path, changed) < 0)
	    result = 1;
	entry = next;
    }
    if (dedupe_tree(db, prefix, changed, &result) < 0)
	result = 1;
    return result;
}
static int walk(OverlayDb *db, const char *prefix, bool merge, bool verify,
		bool recursive)
{
    OverlayEntry *entry;
    int result = 0;
    size_t prefix_length = strlen(prefix);

    for (entry = db->entries; entry != NULL; ) {
	OverlayEntry *next = entry->next;
	bool matches = strcmp(entry->path, prefix) == 0
	    || (prefix_length == 0)
	    || (strncmp(entry->path, prefix, prefix_length) == 0
		&& entry->path[prefix_length] == '/');

	if (!matches) {
	    entry = next;
	    continue;
	}
	if (entry->deleted) {
	    if (!merge)
		printf("%s deleted\n", entry->path);
	    entry = next;
	    continue;
	}
	if (!entry->block) {
	    if (!merge && !recursive)
		result = check_entry(db, entry, entry->path) ? 1 : result;
	    entry = next;
	    continue;
	}
	if (merge) {
	    int status = merge_entry(db, entry, verify, entry->path);

	    if (status < 0)
		return status;
	    if (save_helper_db(db) < 0)
		return -EIO;
	} else {
	    int status = check_entry(db, entry, entry->path);

	    if (status == 2)
		return 2;
	    if (status == 1)
		result = 1;
	}
	entry = next;
    }
    return result;
}

int main(int argc, char **argv)
{
    const char *name;
    const char *path = NULL;
    bool recursive = false;
    bool verify = false;
    bool merge;
    bool dedupe;
    OverlayDb db;
    char rel[OVERLAY_PATH_MAX];
    OverlayEntry *entry;
    int status;

    name = strrchr(argv[0], '/');
    name = name != NULL ? name + 1 : argv[0];
    merge = strcmp(name, "bme") == 0;
    dedupe = strcmp(name, "brd") == 0;

    for (int i = 1; i < argc; i++) {
	if (strcmp(argv[i], "-r") == 0)
	    recursive = true;
	else if (strcmp(argv[i], "-h") == 0)
	    verify = true;
	else if (argv[i][0] == '-') {
	    usage(name);
	    return 2;
	} else if (path == NULL) {
	    path = argv[i];
	} else {
	    usage(name);
	    return 2;
	}
    }
    if (path == NULL) {
	usage(name);
	return 2;
    }
    if (!merge && verify) {
	usage(name);
	return 2;
    }

    status = overlay_db_load(&db, DB_VIEW);
    if (status < 0) {
	fprintf(stderr, "%s: cannot read %s: %s\n", name, DB_VIEW,
		strerror(-status));
	return 2;
    }
    /* The database stores host paths; the helper works through the bind
     * views, which are valid inside the container.  Keep the original
     * header values so saving does not rewrite them.  */
    host_upper = db.upper;
    host_lower = db.lower;
    db.upper = NULL;
    db.lower = NULL;
    db.upper = strdup(UPPER_VIEW);
    db.lower = strdup(LOWER_VIEW);
    if (db.upper == NULL || db.lower == NULL) {
	overlay_db_free(&db);
	free(host_upper);
	free(host_lower);
	return 2;
    }

    status = rel_from_guest(&db, path, rel, sizeof(rel));
    if (status < 0) {
	fprintf(stderr, "%s: %s is not inside the overlay mount\n", name,
		path);
	overlay_db_free(&db);
	return 2;
    }

    if (dedupe) {
	char guest_path[OVERLAY_PATH_MAX];
	struct stat st;
	bool changed = false;
	int result = 0;

	if (brd_private(rel)) {
	    fprintf(stderr, "brd: %s: hidden overlay path\n", rel);
	    overlay_db_free(&db);
	    return 1;
	}
	if (db.read_only) {
	    fprintf(stderr, "brd: read-only overlay\n");
	    overlay_db_free(&db);
	    return 1;
	}
	if (db.upper_source) {
	    fprintf(stderr,
		    "brd: type 1 is a source; run brd on a directory-backed "
		    "type 1\n");
	    overlay_db_free(&db);
	    return 1;
	}
	if (overlay_join(guest_path, sizeof(guest_path), db.guest, rel) < 0) {
	    overlay_db_free(&db);
	    return 1;
	}
	if (recursive && stat(guest_path, &st) == 0 && S_ISDIR(st.st_mode)) {
	    result = dedupe_walk(&db, rel, &changed);
	} else if (!recursive && stat(guest_path, &st) == 0
		   && S_ISDIR(st.st_mode)) {
	    fprintf(stderr, "brd: %s is a directory (use -r)\n", rel);
	    overlay_db_free(&db);
	    return 1;
	} else {
	    result = dedupe_path(&db, rel, &changed) < 0 ? 1 : 0;
	}
	if (changed && save_helper_db(&db) < 0)
	    result = 1;
	overlay_db_free(&db);
	return result;
    }

    if (recursive) {
	status = walk(&db, rel, merge, verify, true);
	if (status < 0) {
	    overlay_db_free(&db);
	    return 1;
	}
	overlay_db_free(&db);
	return status == 2 ? 2 : status;
    }

    entry = overlay_entry_find(&db, rel);
    if (!merge) {
	status = check_entry(&db, entry, rel);
	overlay_db_free(&db);
	return status == 2 ? 2 : status;
    }

    if (entry != NULL && entry->deleted) {
	fprintf(stderr, "bme: %s: no such file\n", rel);
	overlay_db_free(&db);
	return 1;
    }
    if (entry == NULL || !entry->block) {
	printf("merged %s\n", rel);
	overlay_db_free(&db);
	return 0;
    }
    status = merge_entry(&db, entry, verify, rel);
    if (status == 0 && save_helper_db(&db) < 0)
	status = -EIO;
    overlay_db_free(&db);
    return status < 0 ? 1 : 0;
}
