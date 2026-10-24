/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Text database and small helpers shared by the overlay filesystem and
 * the /dev/bkc + /dev/bme commands.
 *
 * The database deliberately uses plain malloc: overlayctl is a small
 * static helper and must not depend on talloc or on the uvroot runtime.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "extension/overlay/overlay_db.h"
#include "extension/overlay/overlay_sha256.h"

static char *xstrdup(const char *value)
{
    char *copy;
    size_t length;

    if (value == NULL)
	return NULL;
    length = strlen(value) + 1;
    copy = malloc(length);
    if (copy != NULL)
	memcpy(copy, value, length);
    return copy;
}

static void db_escape(const char *input, char *output, size_t size)
{
    size_t used = 0;

    for (; *input != '\0'; input++) {
	const char *replacement = NULL;

	switch (*input) {
	case '\\':
	    replacement = "\\\\";
	    break;
	case '\n':
	    replacement = "\\n";
	    break;
	case '\t':
	    replacement = "\\t";
	    break;
	default:
	    break;
	}
	if (replacement != NULL) {
	    if (used + 2 >= size)
		break;
	    output[used++] = replacement[0];
	    output[used++] = replacement[1];
	} else {
	    if (used + 1 >= size)
		break;
	    output[used++] = *input;
	}
    }
    if (size > 0)
	output[used < size ? used : size - 1] = '\0';
}

static void db_unescape(const char *input, char *output, size_t size)
{
    size_t used = 0;

    while (*input != '\0' && used + 1 < size) {
	if (input[0] == '\\' && input[1] != '\0') {
	    switch (input[1]) {
	    case 'n':
		output[used++] = '\n';
		break;
	    case 't':
		output[used++] = '\t';
		break;
	    case '\\':
		output[used++] = '\\';
		break;
	    default:
		output[used++] = input[1];
		break;
	    }
	    input += 2;
	    continue;
	}
	output[used++] = *input++;
    }
    if (size > 0)
	output[used < size ? used : size - 1] = '\0';
}

static void ranges_free(OverlayRange *range)
{
    while (range != NULL) {
	OverlayRange *next = range->next;

	free(range);
	range = next;
    }
}

void overlay_db_free(OverlayDb *db)
{
    OverlayEntry *entry;

    if (db == NULL)
	return;
    entry = db->entries;
    while (entry != NULL) {
	OverlayEntry *next = entry->next;

	free(entry->path);
	ranges_free(entry->ranges);
	free(entry);
	entry = next;
    }
    free(db->path);
    free(db->upper);
    free(db->lower);
    free(db->guest);
    memset(db, 0, sizeof(*db));
}

OverlayEntry *overlay_entry_find(OverlayDb *db, const char *rel)
{
    OverlayEntry *entry;

    for (entry = db->entries; entry != NULL; entry = entry->next) {
	if (strcmp(entry->path, rel) == 0)
	    return entry;
    }
    return NULL;
}

OverlayEntry *overlay_entry_get(OverlayDb *db, const char *rel, bool create)
{
    OverlayEntry *entry = overlay_entry_find(db, rel);

    if (entry != NULL || !create)
	return entry;
    entry = calloc(1, sizeof(*entry));
    if (entry == NULL)
	return NULL;
    entry->path = xstrdup(rel);
    if (entry->path == NULL) {
	free(entry);
	return NULL;
    }
    entry->block_size = OVERLAY_BLOCK_SIZE;
    entry->next = db->entries;
    db->entries = entry;
    return entry;
}

void overlay_entry_remove(OverlayDb *db, const char *rel)
{
    OverlayEntry **link = &db->entries;

    while (*link != NULL) {
	OverlayEntry *entry = *link;

	if (strcmp(entry->path, rel) == 0) {
	    *link = entry->next;
	    free(entry->path);
	    ranges_free(entry->ranges);
	    free(entry);
	    continue;
	}
	link = &entry->next;
    }
}

void overlay_entry_remove_tree(OverlayDb *db, const char *prefix)
{
    size_t length = strlen(prefix);
    OverlayEntry **link = &db->entries;

    while (*link != NULL) {
	OverlayEntry *entry = *link;

	if (strcmp(entry->path, prefix) == 0
	    || (strncmp(entry->path, prefix, length) == 0
		&& entry->path[length] == '/')) {
	    *link = entry->next;
	    free(entry->path);
	    ranges_free(entry->ranges);
	    free(entry);
	    continue;
	}
	link = &entry->next;
    }
}

void overlay_entry_drop_duplicates(OverlayDb *db)
{
    OverlayEntry *entry;

    for (entry = db->entries; entry != NULL; entry = entry->next) {
	OverlayEntry **link = &entry->next;

	while (*link != NULL) {
	    OverlayEntry *candidate = *link;

	    if (strcmp(candidate->path, entry->path) == 0) {
		*link = candidate->next;
		free(candidate->path);
		ranges_free(candidate->ranges);
		free(candidate);
		continue;
	    }
	    link = &candidate->next;
	}
    }
}

/* A path is hidden when it, or one of its ancestors, is a whiteout.  */
bool overlay_db_hidden(OverlayDb *db, const char *rel)
{
    char prefix[OVERLAY_PATH_MAX];
    size_t length = strlen(rel);
    OverlayEntry *entry;

    if (length >= sizeof(prefix))
	return false;
    memcpy(prefix, rel, length + 1);

    for (;;) {
	char *slash;

	entry = overlay_entry_find(db, prefix);
	if (entry != NULL && entry->deleted)
	    return true;
	slash = strrchr(prefix, '/');
	if (slash == NULL)
	    return false;
	*slash = '\0';
    }
}

static void range_append(OverlayEntry *entry, uint64_t offset, uint64_t length)
{
    OverlayRange *range;
    OverlayRange *previous = NULL;
    OverlayRange **tail;

    for (tail = &entry->ranges; *tail != NULL; tail = &(*tail)->next)
	previous = *tail;
    if (previous != NULL
	&& previous->offset + previous->length == offset) {
	previous->length += length;
	return;
    }

    range = calloc(1, sizeof(*range));
    if (range == NULL)
	return;
    range->offset = offset;
    range->length = length;
    *tail = range;
}

static int db_parse_entry(OverlayDb *db, char *line)
{
    char *fields[8];
    char *cursor;
    int count = 0;
    OverlayEntry *entry;
    char decoded[OVERLAY_PATH_MAX];

    cursor = line;
    while (count < 8) {
	fields[count++] = cursor;
	cursor = strchr(cursor, '\t');
	if (cursor == NULL)
	    break;
	*cursor++ = '\0';
    }
    if (count < 2)
	return -EINVAL;

    if (strcmp(fields[0], "D") == 0) {
	db_unescape(fields[1], decoded, sizeof(decoded));
	entry = overlay_entry_get(db, decoded, true);
	if (entry == NULL)
	    return -ENOMEM;
	entry->deleted = true;
	entry->block = false;
	return 0;
    }

    if (strcmp(fields[0], "B") != 0 || count < 8)
	return -EINVAL;

    db_unescape(fields[1], decoded, sizeof(decoded));
    entry = overlay_entry_get(db, decoded, true);
    if (entry == NULL)
	return -ENOMEM;
    entry->deleted = false;
    entry->block = true;
    entry->size = strtoull(fields[2], NULL, 10);
    entry->mtime = (time_t) strtoll(fields[3], NULL, 10);
    entry->mode = (mode_t) strtoul(fields[4], NULL, 8);
    entry->block_size = (uint32_t) strtoul(fields[5], NULL, 10);
    if (entry->block_size == 0)
	entry->block_size = OVERLAY_BLOCK_SIZE;
    snprintf(entry->hash, sizeof(entry->hash), "%s", fields[6]);

    cursor = fields[7];
    while (*cursor != '\0') {
	char *end;
	uint64_t offset;
	uint64_t length;

	offset = strtoull(cursor, &end, 10);
	if (end == cursor || *end != ':')
	    break;
	cursor = end + 1;
	length = strtoull(cursor, &end, 10);
	if (end == cursor)
	    break;
	range_append(entry, offset, length);
	if (*end == ',')
	    cursor = end + 1;
	else
	    cursor = end;
    }
    return 0;
}

int overlay_db_load(OverlayDb *db, const char *path)
{
    FILE *file;
    char *line = NULL;
    size_t line_capacity = 0;

    memset(db, 0, sizeof(*db));
    db->path = xstrdup(path);
    if (db->path == NULL)
	return -ENOMEM;
    db->mode = OVERLAY_MODE_BIND;

    file = fopen(path, "r");
    if (file == NULL) {
	if (errno == ENOENT)
	    return 0;
	return -errno;
    }

    while (getline(&line, &line_capacity, file) >= 0) {
	char *newline = strchr(line, '\n');

	if (newline != NULL)
	    *newline = '\0';
	if (line[0] == '#' || line[0] == '\0')
	    continue;

	if (strncmp(line, "mode ", 5) == 0) {
	    const char *value = line + 5;

	    if (strcmp(value, "block") == 0)
		db->mode = OVERLAY_MODE_BLOCK;
	    else if (strcmp(value, "bind") == 0)
		db->mode = OVERLAY_MODE_BIND;
	    else
		goto malformed;
	} else if (strncmp(line, "upper ", 6) == 0) {
	    char decoded[OVERLAY_PATH_MAX];

	    db_unescape(line + 6, decoded, sizeof(decoded));
	    free(db->upper);
	    db->upper = xstrdup(decoded);
	} else if (strncmp(line, "lower ", 6) == 0) {
	    char decoded[OVERLAY_PATH_MAX];

	    db_unescape(line + 6, decoded, sizeof(decoded));
	    free(db->lower);
	    db->lower = xstrdup(decoded);
	} else if (strncmp(line, "guest ", 6) == 0) {
	    char decoded[OVERLAY_PATH_MAX];

	    db_unescape(line + 6, decoded, sizeof(decoded));
	    free(db->guest);
	    db->guest = xstrdup(decoded);
	} else if (strncmp(line, "read_only ", 10) == 0) {
	    db->read_only = strtoul(line + 10, NULL, 10) != 0;
	} else if (strncmp(line, "upper_source ", 13) == 0) {
	    db->upper_source = strtoul(line + 13, NULL, 10) != 0;
	} else if (line[0] == 'D' || line[0] == 'B') {
	    if (db_parse_entry(db, line) < 0)
		goto malformed;
	} else {
	    goto malformed;
	}
    }
    free(line);
    fclose(file);
    overlay_entry_drop_duplicates(db);
    return 0;

  malformed:
    free(line);
    fclose(file);
    overlay_db_free(db);
    return -EINVAL;
}

int overlay_db_save(OverlayDb *db)
{
    char temporary[OVERLAY_PATH_MAX + 64];
    FILE *file;
    OverlayEntry *entry;

    if (db->path == NULL)
	return -EINVAL;
    snprintf(temporary, sizeof(temporary), "%s.tmp", db->path);
    unlink(temporary);
    file = fopen(temporary, "w");
    if (file == NULL)
	return -errno;

    fprintf(file, "# uvroot overlay database v1\n");
    fprintf(file, "mode %s\n",
	    db->mode == OVERLAY_MODE_BLOCK ? "block" : "bind");
    {
	char escaped[OVERLAY_PATH_MAX * 2];

	db_escape(db->guest != NULL ? db->guest : "", escaped,
		  sizeof(escaped));
	fprintf(file, "guest %s\n", escaped);
	db_escape(db->upper != NULL ? db->upper : "", escaped,
		  sizeof(escaped));
	fprintf(file, "upper %s\n", escaped);
	db_escape(db->lower != NULL ? db->lower : "", escaped,
		  sizeof(escaped));
	fprintf(file, "lower %s\n", escaped);
    }
    fprintf(file, "read_only %d\n", db->read_only ? 1 : 0);
    fprintf(file, "upper_source %d\n", db->upper_source ? 1 : 0);

    for (entry = db->entries; entry != NULL; entry = entry->next) {
	char escaped[OVERLAY_PATH_MAX * 2];

	db_escape(entry->path, escaped, sizeof(escaped));
	if (entry->deleted) {
	    fprintf(file, "D\t%s\n", escaped);
	    continue;
	}
	if (!entry->block)
	    continue;

	fprintf(file, "B\t%s\t%llu\t%lld\t%o\t%u\t%s\t",
		escaped, (unsigned long long) entry->size,
		(long long) entry->mtime, (unsigned) entry->mode,
		entry->block_size, entry->hash);
	{
	    OverlayRange *range;
	    bool first = true;

	    for (range = entry->ranges; range != NULL; range = range->next) {
		fprintf(file, "%s%llu:%llu", first ? "" : ",",
			(unsigned long long) range->offset,
			(unsigned long long) range->length);
		first = false;
	    }
	}
	fprintf(file, "\n");
    }

    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
	int status = -errno;

	fclose(file);
	unlink(temporary);
	return status;
    }
    fclose(file);
    if (rename(temporary, db->path) < 0) {
	int status = -errno;

	unlink(temporary);
	return status;
    }
    return 0;
}

int overlay_join(char *out, size_t size, const char *base, const char *rel)
{
    int length;

    if (rel == NULL || rel[0] == '\0')
	length = snprintf(out, size, "%s", base);
    else
	length = snprintf(out, size, "%s/%s", base, rel);
    if (length < 0 || (size_t) length >= size)
	return -ENAMETOOLONG;
    return 0;
}

int overlay_hash_file(const char *path, char hex[65])
{
    unsigned char buffer[65536];
    OverlaySha256 ctx;
    unsigned char digest[32];
    ssize_t got;
    int fd;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
	return -errno;
    overlay_sha256_init(&ctx);
    while ((got = read(fd, buffer, sizeof(buffer))) > 0)
	overlay_sha256_update(&ctx, buffer, (size_t) got);
    if (got < 0) {
	int status = -errno;

	close(fd);
	return status;
    }
    close(fd);
    overlay_sha256_final(&ctx, digest);
    overlay_sha256_hex(digest, hex);
    return 0;
}

int overlay_hash_fd(int fd, char hex[65])
{
    unsigned char buffer[65536];
    OverlaySha256 ctx;
    unsigned char digest[32];
    ssize_t got;
    off_t saved = lseek(fd, 0, SEEK_CUR);

    if (saved < 0)
	return -errno;
    if (lseek(fd, 0, SEEK_SET) < 0)
	return -errno;
    overlay_sha256_init(&ctx);
    while ((got = read(fd, buffer, sizeof(buffer))) > 0)
	overlay_sha256_update(&ctx, buffer, (size_t) got);
    if (got < 0) {
	int status = -errno;

	(void) lseek(fd, saved, SEEK_SET);
	return status;
    }
    (void) lseek(fd, saved, SEEK_SET);
    overlay_sha256_final(&ctx, digest);
    overlay_sha256_hex(digest, hex);
    return 0;
}

int overlay_copy_file(const char *from, const char *to, mode_t mode)
{
    char temporary[OVERLAY_PATH_MAX + 64];
    unsigned char buffer[65536];
    ssize_t got;
    int in;
    int out;
    int status = 0;

    in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0)
	return -errno;
    snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", to, (long) getpid());
    unlink(temporary);
    out = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
	       mode & 07777);
    if (out < 0) {
	status = -errno;
	close(in);
	return status;
    }
    (void) fchmod(out, mode & 07777);
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
  out:
    close(in);
    if (close(out) < 0 && status == 0)
	status = -errno;
    if (status < 0) {
	unlink(temporary);
	return status;
    }
    if (rename(temporary, to) < 0) {
	status = -errno;
	unlink(temporary);
    }
    return status;
}

/*
 * Build the merged view of a block entry in @destination.  The caller
 * may pass @destination == @upper only when it has already read the
 * previous delta, so the normal path uses a temporary file.
 */
int overlay_delta_merge(OverlayDb *db, const OverlayEntry *entry,
			const char *upper, const char *lower,
			const char *destination)
{
    char lower_path[OVERLAY_PATH_MAX];
    char upper_path[OVERLAY_PATH_MAX];
    char temporary[OVERLAY_PATH_MAX + 64];
    unsigned char buffer[65536];
    OverlayRange *range;
    ssize_t got;
    int in = -1;
    int delta = -1;
    int out = -1;
    int status = 0;

    if (overlay_join(lower_path, sizeof(lower_path), db->lower, entry->path) < 0
	|| overlay_join(upper_path, sizeof(upper_path), db->upper,
			entry->path) < 0)
	return -ENAMETOOLONG;
    if (lower != NULL)
	snprintf(lower_path, sizeof(lower_path), "%s", lower);
    if (upper != NULL)
	snprintf(upper_path, sizeof(upper_path), "%s", upper);

    if (snprintf(temporary, sizeof(temporary), "%s.merge.%ld", destination,
		 (long) getpid()) >= (int) sizeof(temporary))
	return -ENAMETOOLONG;
    unlink(temporary);
    out = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
	       (entry->mode & 07777) | S_IRUSR | S_IWUSR);
    if (out < 0)
	return -errno;

    in = open(lower_path, O_RDONLY | O_CLOEXEC);
    if (in >= 0) {
	while ((got = read(in, buffer, sizeof(buffer))) > 0) {
	    ssize_t done = 0;

	    while (done < got) {
		ssize_t written = write(out, buffer + done,
					(size_t) (got - done));

		if (written < 0) {
		    status = -errno;
		    goto fail;
		}
		done += written;
	    }
	}
	if (got < 0) {
	    status = -errno;
	    goto fail;
	}
	close(in);
	in = -1;
    } else if (errno != ENOENT) {
	status = -errno;
	goto fail;
    }

    delta = open(upper_path, O_RDONLY | O_CLOEXEC);
    if (delta < 0 && errno != ENOENT) {
	status = -errno;
	goto fail;
    }
    for (range = delta >= 0 ? entry->ranges : NULL; range != NULL;
	 range = range->next) {
	uint64_t left = range->length;
	uint64_t offset = range->offset;

	while (left > 0) {
	    size_t chunk = left < sizeof(buffer) ? (size_t) left : sizeof(buffer);
	    ssize_t done = 0;

	    got = pread(delta, buffer, chunk, (off_t) offset);
	    if (got < 0) {
		status = -errno;
		goto fail;
	    }
	    if (got == 0)
		break;
	    while (done < got) {
		ssize_t written = pwrite(out, buffer + done,
					 (size_t) (got - done),
					 (off_t) (offset + (uint64_t) done));

		if (written < 0) {
		    status = -errno;
		    goto fail;
		}
		done += written;
	    }
	    offset += (uint64_t) got;
	    left -= (uint64_t) got;
	}
    }
    if (delta >= 0)
	close(delta);
    delta = -1;

    if (ftruncate(out, (off_t) entry->size) < 0) {
	status = -errno;
	goto fail;
    }
    if (fchmod(out, entry->mode & 07777) < 0)
	status = -errno;
    if (close(out) < 0 && status == 0)
	status = -errno;
    out = -1;
    if (status < 0) {
	unlink(temporary);
	return status;
    }
    if (rename(temporary, destination) < 0) {
	status = -errno;
	unlink(temporary);
    }
    return status;

  fail:
    if (in >= 0)
	close(in);
    if (delta >= 0)
	close(delta);
    if (out >= 0)
	close(out);
    unlink(temporary);
    return status;
}

int overlay_effective_path(OverlayDb *db, const char *rel, char *out,
			   size_t size, bool *from_upper)
{
    struct stat st;
    char path[OVERLAY_PATH_MAX];

    if (overlay_db_hidden(db, rel))
	return -ENOENT;
    if (overlay_join(path, sizeof(path), db->upper, rel) < 0)
	return -ENAMETOOLONG;
    if (lstat(path, &st) == 0) {
	if (out != NULL)
	    snprintf(out, size, "%s", path);
	if (from_upper != NULL)
	    *from_upper = true;
	return 0;
    }
    if (errno != ENOENT)
	return -errno;
    if (overlay_join(path, sizeof(path), db->lower, rel) < 0)
	return -ENAMETOOLONG;
    if (lstat(path, &st) == 0) {
	if (out != NULL)
	    snprintf(out, size, "%s", path);
	if (from_upper != NULL)
	    *from_upper = false;
	return 0;
    }
    return errno == ENOENT ? -ENOENT : -errno;
}
