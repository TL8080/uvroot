/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * bkc / bme: inspect and permanently merge overlay block deltas.
 *
 * This is a small static helper bound to /dev/bkc and /dev/bme.  It
 * runs inside the container, so geteuid() and stat(2) go through vperm
 * when --vperm is enabled; that is how the merge permission check sees
 * the virtual identity and the virtual file owner.
 */
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
	    "       %s [-h] [-r] PATH\n", name, name);
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

static int walk(OverlayDb *db, const char *prefix, bool merge, bool verify,
		bool recursive)
{
    OverlayEntry *entry;
    int result = 0;
    size_t prefix_length = strlen(prefix);

    for (entry = db->entries; entry != NULL; entry = entry->next) {
	bool matches = strcmp(entry->path, prefix) == 0
	    || (prefix_length == 0)
	    || (strncmp(entry->path, prefix, prefix_length) == 0
		&& entry->path[prefix_length] == '/');

	if (!matches)
	    continue;
	if (entry->deleted) {
	    if (!merge)
		printf("%s deleted\n", entry->path);
	    continue;
	}
	if (!entry->block) {
	    if (!merge && !recursive)
		result = check_entry(db, entry, entry->path) ? 1 : result;
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
    OverlayDb db;
    char rel[OVERLAY_PATH_MAX];
    OverlayEntry *entry;
    int status;

    name = strrchr(argv[0], '/');
    name = name != NULL ? name + 1 : argv[0];
    merge = strcmp(name, "bme") == 0;

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
