/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OVERLAY_DB_H
#define OVERLAY_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define OVERLAY_DB_NAME ".overlay"
#define OVERLAY_PATH_MAX 4096
#define OVERLAY_BLOCK_SIZE 4096

typedef enum OverlayMode {
    OVERLAY_MODE_BIND = 0,
    OVERLAY_MODE_BLOCK = 1,
} OverlayMode;

typedef struct OverlayRange {
    uint64_t offset;
    uint64_t length;
    struct OverlayRange *next;
} OverlayRange;

typedef struct OverlayEntry {
    struct OverlayEntry *next;
    char *path;			/* relative, no leading slash */
    bool deleted;		/* whiteout */
    bool block;			/* type1 is a sparse delta */
    uint64_t size;		/* logical size */
    time_t mtime;
    mode_t mode;
    uint32_t block_size;
    char hash[65];
    OverlayRange *ranges;
} OverlayEntry;

typedef struct OverlayDb {
    OverlayMode mode;
    char *path;			/* host path of the .overlay file */
    char *upper;		/* host absolute path */
    char *lower;		/* host absolute path */
    char *guest;		/* guest mount point */
    bool read_only;		/* merge commands must refuse writes */
    bool upper_source;		/* type 1 is backed by a non-directory source */
    OverlayEntry *entries;
} OverlayDb;

int overlay_db_load(OverlayDb *db, const char *path);
int overlay_db_save(OverlayDb *db);
void overlay_db_free(OverlayDb *db);

OverlayEntry *overlay_entry_find(OverlayDb *db, const char *rel);
OverlayEntry *overlay_entry_get(OverlayDb *db, const char *rel, bool create);
void overlay_entry_remove(OverlayDb *db, const char *rel);
void overlay_entry_remove_tree(OverlayDb *db, const char *prefix);
void overlay_entry_drop_duplicates(OverlayDb *db);
bool overlay_db_hidden(OverlayDb *db, const char *rel);

int overlay_join(char *out, size_t size, const char *base, const char *rel);
int overlay_hash_file(const char *path, char hex[65]);
int overlay_hash_fd(int fd, char hex[65]);
int overlay_copy_file(const char *from, const char *to, mode_t mode);
int overlay_delta_merge(OverlayDb *db, const OverlayEntry *entry,
			const char *upper, const char *lower,
			const char *destination);
int overlay_effective_path(OverlayDb *db, const char *rel, char *out,
			   size_t size, bool *from_upper);

#endif
