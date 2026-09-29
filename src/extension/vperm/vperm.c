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
 * vperm: persistent virtual ownership/permissions for a guest root.
 *
 * A small text database stored in (or beside) the guest root records a
 * virtual uid, gid and mode for a subset of the paths.  Programs inside
 * the guest see those values through the stat(2) family, chmod(2) and
 * chown(2) never touch the host but only update the database, and
 * access(2)/open(2)/execve(2) are checked against the recorded mode.
 * Paths without an entry keep the host's own metadata ("pass through").
 *
 * The database is never modified behind the guest's back: it is only
 * changed by the guest's own metadata calls, by file creation, and by
 * unlink/rmdir/rename, and it is written back atomically so a crash
 * cannot leave it half-written.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <talloc.h>

#include "build.h"
#include "cli/note.h"
#include "extension/extension.h"
#include "extension/netfs/netfs.h"
#include "extension/vperm/vperm.h"
#include "path/binding.h"
#include "path/path.h"
#include "syscall/seccomp.h"
#include "syscall/syscall.h"
#include "syscall/sysnum.h"
#include "tracee/abi.h"
#include "tracee/mem.h"
#include "tracee/reg.h"
#include "arch.h"

#define VPERM_DB_NAME		".uvroot-vperm"

/*
 * Database name used by upstream PRoot.  It is taken over when the
 * current name is absent, so that a rootfs set up for PRoot keeps its
 * recorded virtual ownership.
 */
static const char *const vperm_db_legacy[] = {
    ".proot-vperm",
    NULL,
};

/* Linux directory entry layouts, as seen by the tracee.  */
typedef struct VpermDirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
} VpermDirent64;

typedef struct VpermDirent {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    char d_name[];
} VpermDirent;

typedef struct VpermEntry {
    struct VpermEntry *next;
    char *path;			/* relative to the root, no leading '/' */
    mode_t mode;
    uid_t uid;
    gid_t gid;
} VpermEntry;

typedef struct VpermRoot {
    struct VpermRoot *next;
    char *host;			/* canonical host path, no trailing '/' */
    char *guest;		/* guest mount point */
    char *db;			/* host path of the database file */
    char *db_rel;		/* database path relative to @host, or NULL */
    VpermEntry *entries;
    bool local;			/* false when the root is a netfs cache */
    bool fs_backed;		/* the root has its own filesystem metadata */
    bool loaded;
    /*
     * Identity of the database file.  The container must never be able
     * to read or change the database, and a path comparison alone is not
     * enough: a hard link created before --vperm was enabled -- or from
     * the host -- reaches the same inode under a different name.
     */
    bool db_id;			/* @db_dev/@db_ino are valid */
    dev_t db_dev;
    ino_t db_ino;
} VpermRoot;

typedef struct VpermFd {
    struct VpermFd *next;
    int fd;
    VpermRoot *root;
    char rel[PATH_MAX];
} VpermFd;

typedef struct VpermTracee {
    struct VpermTracee *next;
    pid_t pid;
    VpermFd *fds;

    /* Set when a metadata syscall was turned into a no-op.  */
    bool emulate;

    char pend_rel[PATH_MAX];
    VpermRoot *pend_root;
    bool pend_valid;

    /* The host path did not exist when the syscall was entered: a
     * successful open(O_CREAT) therefore created a new object, and a
     * stale entry for that path has to be refreshed.  */
    bool pend_absent;

    /* Destination of a rename, and the entry it carries over.  */
    char pend_new_rel[PATH_MAX];
    VpermRoot *pend_new_root;
    bool pend_new_valid;
    mode_t move_mode;
    uid_t move_uid;
    gid_t move_gid;
    bool move_valid;

    /* Current virtual identity of this process; changed by the
     * setuid/setgid family and inherited across fork/exec.  */
    uid_t uid;
    gid_t gid;

    /* Last canonicalized host path seen during this syscall, captured
     * from the HOST_PATH event.  For rename(2) this is the destination,
     * which is not readable from the registers at the exit stage.  */
    char last_rel[PATH_MAX];
    char last_host[PATH_MAX];
    VpermRoot *last_root;
    bool last_fs_root;
    bool last_valid;

    /* The captured path belongs to the (read-only) mapping area.  */
    bool pend_protected;

    /* Captured host path, and whether it lives in a block-backed root
     * whose filesystem owns the metadata.  pend_fs_root is only set when
     * that block mount is the *guest root* (a container booted from the
     * image), where the image metadata is authoritative.  A block data
     * mount is exposed to a guest that runs as the real host user, so
     * the image permissions are informational there.  */
    char pend_host[PATH_MAX];
    bool pend_fs_backed;
    bool pend_fs_root;
} VpermTracee;

typedef struct VmapUser {
    struct VmapUser *next;
    char name[64];
    uid_t uid;
    gid_t gid;
    char shell[PATH_MAX];
    char raw[PATH_MAX];		/* original /etc/passwd line, if preserved */
    bool preserved;
} VmapUser;

typedef struct VpermConfig {
    VpermRoot *roots;
    VpermTracee *tracees;
    uid_t uid;
    gid_t gid;
    char *explicit_db;
    bool built;
    int build_status;		/* cached result of roots_build() */

    /* Switchable mapping directory: users.conf plus the generated
     * su/sudo shims that get bound into the container.  */
    char *map_dir;
    VmapUser *users;
    bool shims_ready;
    bool no_shims;		/* --vperm-nosu: no virtual su/sudo */
} VpermConfig;

static void root_db_ident(VpermRoot *root);

#define VMAP_MAGIC "/.uvroot-vperm-switch"

static bool perm_allows(const VpermEntry * entry, uid_t uid, gid_t gid,
			int want);

/* ------------------------------------------------------------------ */
/* Database                                                            */
/* ------------------------------------------------------------------ */

static void db_escape(const char *input, char *output, size_t size)
{
    size_t used = 0;
    const char *cursor;

    for (cursor = input; *cursor != '\0'; cursor++) {
	const char *replacement = NULL;

	switch (*cursor) {
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
	    output[used++] = *cursor;
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

static VpermEntry *entry_find(VpermRoot *root, const char *rel)
{
    VpermEntry *entry;

    for (entry = root->entries; entry != NULL; entry = entry->next) {
	if (strcmp(entry->path, rel) == 0)
	    return entry;
    }

    return NULL;
}

static VpermEntry *entry_get(VpermRoot *root, const char *rel, bool create)
{
    VpermEntry *entry = entry_find(root, rel);

    if (entry != NULL || !create)
	return entry;

    entry = talloc_zero(root, VpermEntry);
    if (entry == NULL)
	return NULL;

    entry->path = talloc_strdup(entry, rel);
    if (entry->path == NULL) {
	talloc_free(entry);
	return NULL;
    }

    entry->next = root->entries;
    root->entries = entry;
    return entry;
}

/*
 * Rewrite every entry below @old_prefix so a renamed directory keeps the
 * metadata of its whole subtree.
 */
static void entry_move_tree(VpermRoot *root, const char *old_prefix,
			    const char *new_prefix)
{
    size_t old_length = strlen(old_prefix);
    VpermEntry *entry;

    for (entry = root->entries; entry != NULL; entry = entry->next) {
	char moved[PATH_MAX];

	if (strcmp(entry->path, old_prefix) == 0)
	    snprintf(moved, sizeof(moved), "%s", new_prefix);
	else if (strncmp(entry->path, old_prefix, old_length) == 0
		 && entry->path[old_length] == '/')
	    snprintf(moved, sizeof(moved), "%s%s", new_prefix,
		     entry->path + old_length);
	else
	    continue;

	{
	    char *replacement = talloc_strdup(entry, moved);

	    if (replacement == NULL)
		continue;
	    talloc_free(entry->path);
	    entry->path = replacement;
	}
    }
}

/* Every ancestor directory of @rel that has an entry must grant x.  */
static int check_ancestors(VpermRoot *root, const char *rel, uid_t uid,
			   gid_t gid)
{
    const char *cursor = rel;

    while ((cursor = strchr(cursor, '/')) != NULL) {
	char prefix[PATH_MAX];
	size_t length = (size_t) (cursor - rel);
	VpermEntry *entry;

	if (length >= sizeof(prefix))
	    break;
	memcpy(prefix, rel, length);
	prefix[length] = '\0';

	entry = entry_find(root, prefix);
	if (entry != NULL && !perm_allows(entry, uid, gid, X_OK))
	    return -EACCES;

	cursor++;
    }

    return 0;
}

/* Creating or removing @rel requires w and x on its parent.  */
static int check_parent_write(VpermRoot *root, const char *rel, uid_t uid,
			      gid_t gid)
{
    const char *slash = strrchr(rel, '/');
    char parent[PATH_MAX];
    VpermEntry *entry;

    if (slash == NULL)
	parent[0] = '\0';
    else {
	size_t length = (size_t) (slash - rel);

	if (length >= sizeof(parent))
	    return 0;
	memcpy(parent, rel, length);
	parent[length] = '\0';
    }

    entry = entry_find(root, parent);
    if (entry == NULL)
	return 0;

    if (!perm_allows(entry, uid, gid, W_OK | X_OK))
	return -EACCES;

    return check_ancestors(root, rel, uid, gid);
}

/* Remove the entry @name from the tracee's getdents buffer.  */
static void filter_dirent(Tracee *tracee, const char *name, word_t address,
			  int count, bool is64, bool is_32on64)
{
    size_t reclen_offset = is64 ? 16 : (is_32on64 ? 8 : 16);
    size_t name_offset = is64 ? 19 : (is_32on64 ? 10 : 18);
    char *buffer;
    int offset = 0;
    int kept = 0;

    if (count <= 0 || count > (int) (16 * 1024 * 1024))
	return;

    buffer = malloc((size_t) count);
    if (buffer == NULL)
	return;

    if (read_data(tracee, buffer, address, (word_t) count) < 0) {
	free(buffer);
	return;
    }

    while (offset + (int) name_offset < count) {
	uint16_t entry_length;
	const char *entry_name;

	memcpy(&entry_length, buffer + offset + reclen_offset,
	       sizeof(entry_length));
	if (entry_length == 0 || offset + entry_length > count)
	    break;

	entry_name = buffer + offset + name_offset;
	if (strcmp(entry_name, name) != 0) {
	    if (kept != offset)
		memmove(buffer + kept, buffer + offset, entry_length);
	    kept += entry_length;
	}

	offset += entry_length;
    }

    if (kept != count) {
	if (kept > 0)
	    (void) write_data(tracee, address, buffer, (word_t) kept);
	poke_reg(tracee, SYSARG_RESULT, (word_t) kept);
    }

    free(buffer);
}

static int db_save(VpermRoot *root)
{
    char temporary[PATH_MAX];
    VpermEntry *entry;
    FILE *file;
    int fd;
    int attempt;

    snprintf(temporary, sizeof(temporary), "%s.tmp", root->db);

    /*
     * The container is not allowed to touch the database, but <db>.tmp
     * is not covered by that name check.  Without O_NOFOLLOW a symlink
     * planted there would make this write truncate whatever it points at
     * on the host, so drop any such entry first and refuse to follow a
     * symlink; O_EXCL keeps a concurrent replant from being followed too.
     */
    fd = -1;
    for (attempt = 0; attempt < 3; attempt++) {
	unlink(temporary);
	fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd >= 0 || errno != EEXIST)
	    break;
    }
    if (fd < 0)
	return -errno;

    file = fdopen(fd, "w");
    if (file == NULL) {
	int status = -errno;

	close(fd);
	unlink(temporary);
	return status;
    }

    fprintf(file, "# uvroot vperm database - virtual ownership and permissions\n");

    for (entry = root->entries; entry != NULL; entry = entry->next) {
	char escaped[PATH_MAX * 2];

	db_escape(entry->path, escaped, sizeof(escaped));
	/* The file type is part of the mode and must round-trip: a
	 * stored mode without S_IFDIR makes every directory look like a
	 * regular file to stat(2), which breaks rm/rsync/find.  */
	fprintf(file, "%o %u %u\t%s\n", (unsigned) entry->mode,
		(unsigned) entry->uid, (unsigned) entry->gid, escaped);
    }

    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
	int status = -errno;
	fclose(file);
	unlink(temporary);
	return status;
    }
    fclose(file);

    if (rename(temporary, root->db) < 0) {
	int status = -errno;
	unlink(temporary);
	return status;
    }

    /* rename(2) installs a new inode: refresh the identity we protect. */
    root_db_ident(root);

    return 0;
}

/*
 * Load the database.  A missing one is created empty; one that cannot be
 * read, or that contains anything unexpected, is fatal.  Silently
 * skipping a damaged entry would drop the virtual ownership it records,
 * so uvroot refuses to start instead.
 */
static int db_load(Tracee *tracee, VpermRoot *root)
{
    char line[PATH_MAX * 4];
    FILE *file;
    unsigned int lineno = 0;

    root->loaded = true;

    file = fopen(root->db, "r");
    if (file == NULL) {
	if (errno == ENOENT) {
	    /* "There is one? use it.  None? create it."  A read-only
	     * root cannot hold a database, so a failure here is not
	     * fatal: the entries just stay in memory.  */
	    (void) db_save(root);
	    return 0;
	}

	note(tracee, ERROR, USER, "vperm: cannot read %s: %s",
	     root->db, strerror(errno));
	return -errno;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
	unsigned mode;
	unsigned uid;
	unsigned gid;
	char *path;
	char *newline;
	char decoded[PATH_MAX];
	VpermEntry *entry;
	int consumed = 0;

	lineno++;

	/* A line that does not fit in the buffer is truncated data.  */
	if (strchr(line, '\n') == NULL && !feof(file)) {
	    note(tracee, ERROR, USER, "vperm: %s:%u: line too long",
		 root->db, lineno);
	    fclose(file);
	    return -EINVAL;
	}

	if (line[0] == '#' || line[0] == '\n')
	    continue;

	if (sscanf(line, "%o %u %u%n", &mode, &uid, &gid, &consumed) != 3
	    || consumed <= 0) {
	    note(tracee, ERROR, USER,
		 "vperm: %s:%u: malformed entry", root->db, lineno);
	    fclose(file);
	    return -EINVAL;
	}

	if ((mode & ~(unsigned) (S_IFMT | 07777)) != 0) {
	    note(tracee, ERROR, USER, "vperm: %s:%u: invalid mode %o",
		 root->db, lineno, mode);
	    fclose(file);
	    return -EINVAL;
	}

	/* The separator is a tab, but tolerate spaces too so that a
	 * hand-edited database is not silently ignored.  */
	path = line + consumed;
	while (*path == ' ' || *path == '\t')
	    path++;

	newline = strchr(path, '\n');
	if (newline != NULL)
	    *newline = '\0';

	db_unescape(path, decoded, sizeof(decoded));
	if (decoded[0] == '\0' || decoded[0] == '/') {
	    note(tracee, ERROR, USER, "vperm: %s:%u: invalid path",
		 root->db, lineno);
	    fclose(file);
	    return -EINVAL;
	}

	entry = entry_get(root, decoded, true);
	if (entry == NULL) {
	    fclose(file);
	    return -ENOMEM;
	}
	entry->mode = (mode_t) mode;
	entry->uid = (uid_t) uid;
	entry->gid = (gid_t) gid;

	/* Databases written before type bits were persisted only carry
	 * the permission bits: take the type from the host file.  */
	if ((entry->mode & S_IFMT) == 0) {
	    char full[PATH_MAX];
	    struct stat st;
	    size_t host_length = strlen(root->host);
	    size_t rel_length = strlen(decoded);
	    bool usable = (host_length + 1 + rel_length < sizeof(full));

	    if (usable) {
		memcpy(full, root->host, host_length);
		full[host_length] = '/';
		memcpy(full + host_length + 1, decoded, rel_length + 1);
	    }

	    entry->mode |= (usable && stat(full, &st) == 0)
		? (st.st_mode & S_IFMT) : S_IFREG;
	}
    }

    if (ferror(file)) {
	note(tracee, ERROR, USER, "vperm: read error on %s", root->db);
	fclose(file);
	return -EIO;
    }

    fclose(file);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Mapping directory: users.conf and the su/sudo shims                 */
/* ------------------------------------------------------------------ */

static void mkdir_recursive(const char *path);

static const char *vmap_base(void)
{
    static char base[PATH_MAX];
    const char *environment = getenv("XDG_DATA_HOME");

    if (environment != NULL && environment[0] != '\0')
	snprintf(base, sizeof(base), "%s/uvroot/vperm", environment);
    else {
	const char *home = getenv("HOME");

	if (home == NULL || home[0] == '\0')
	    home = "/tmp";
	snprintf(base, sizeof(base), "%s/.local/share/uvroot/vperm", home);
    }

    return base;
}

static const VmapUser *vmap_find(VpermConfig * config, const char *name);

static void vmap_add_user(VpermConfig *config, const char *name, uid_t uid,
			  gid_t gid, const char *shell, const char *raw)
{
    VmapUser *user = (VmapUser *) vmap_find(config, name);

    /* A name already present is updated, never duplicated: the built-in
     * defaults can therefore be overridden by users.conf.  */
    if (user == NULL) {
	user = talloc_zero(config, VmapUser);
	if (user == NULL)
	    return;
	user->next = config->users;
	config->users = user;
    }

    snprintf(user->name, sizeof(user->name), "%s", name);
    user->uid = uid;
    user->gid = gid;
    snprintf(user->shell, sizeof(user->shell), "%s",
	     shell != NULL && shell[0] != '\0' ? shell : "/bin/sh");

    if (raw != NULL) {
	snprintf(user->raw, sizeof(user->raw), "%s", raw);
	user->preserved = true;
    }
}

static void vmap_clear_users(VpermConfig *config)
{
    while (config->users != NULL) {
	VmapUser *user = config->users;

	config->users = user->next;
	talloc_free(user);
    }
}

static void vmap_load(Tracee *tracee, VpermConfig *config)
{
    char path[PATH_MAX];
    char line[PATH_MAX];
    FILE *file;

    if (config->users != NULL)
	return;

    /* Sane fallbacks so the shims work without any configuration.  */
    vmap_add_user(config, "root", 0, 0, "/bin/sh", NULL);
    vmap_add_user(config, "nobody", 65534, 65534, "/bin/sh", NULL);

    /* Keep the container's own users: the virtual /etc/passwd is a
     * superset, so lookups of bin/daemon/... keep working.  */
    {
	const char *rootfs = get_root(tracee);
	FILE *seed;

	if (rootfs != NULL) {
	    char root[PATH_MAX];

	    snprintf(root, sizeof(root), "%s/etc/passwd", rootfs);
	    seed = fopen(root, "r");
	    if (seed != NULL) {
		while (fgets(line, sizeof(line), seed) != NULL) {
		    char entry[64];
		    unsigned entry_uid;
		    unsigned entry_gid;
		    char entry_shell[PATH_MAX];
		    char *newline;
		    char copy[PATH_MAX];

		    if (line[0] == '#' || line[0] == '\n')
			continue;

		    snprintf(copy, sizeof(copy), "%s", line);
		    newline = strchr(copy, '\n');
		    if (newline != NULL)
			*newline = '\0';

		    entry_shell[0] = '\0';
		    if (sscanf(copy,
			       "%63[^:]:%*[^:]:%u:%u:%*[^:]:%*[^:]:%4095s",
			       entry, &entry_uid, &entry_gid,
			       entry_shell) < 4)
			continue;

		    vmap_add_user(config, entry, (uid_t) entry_uid,
				  (gid_t) entry_gid, entry_shell, copy);
		}
		fclose(seed);
	    }
	}
    }

    snprintf(path, sizeof(path), "%s/users.conf", config->map_dir);
    file = fopen(path, "r");
    if (file == NULL)
	return;

    while (fgets(line, sizeof(line), file) != NULL) {
	char *newline;
	char name[64];
	char shell[PATH_MAX];
	unsigned uid;
	unsigned gid;

	if (line[0] == '#' || line[0] == '\n')
	    continue;

	newline = strchr(line, '\n');
	if (newline != NULL)
	    *newline = '\0';

	shell[0] = '\0';
	if (sscanf(line, "%63[^:]:%u:%u:%4095s", name, &uid, &gid, shell) < 3)
	    continue;

	{
	    VmapUser *existing = (VmapUser *) vmap_find(config, name);

	    vmap_add_user(config, name, (uid_t) uid, (gid_t) gid, shell,
			  NULL);
	    if (existing != NULL)
		existing->preserved = false;
	}
    }

    fclose(file);
}

/*
 * The mapping is materialized as a real /etc/passwd file inside the
 * mapping directory, bound over the container's /etc/passwd: the
 * container can read and write it, and the switches below re-read it so
 * changes are picked up immediately.
 */
static const VmapUser *vmap_find(VpermConfig * config, const char *name);

static void vmap_passwd_path(VpermConfig *config, char *buffer, size_t size)
{
    snprintf(buffer, size, "%s/passwd", config->map_dir);
}

static void vmap_write_passwd(VpermConfig *config)
{
    char path[PATH_MAX];
    FILE *file;

    vmap_passwd_path(config, path, sizeof(path));
    if (access(path, F_OK) == 0)
	return;

    file = fopen(path, "w");
    if (file == NULL)
	return;

    if (config->users == NULL)
	vmap_add_user(config, "root", 0, 0, "/bin/sh", NULL);

    {
	VmapUser *user;

	for (user = config->users; user != NULL; user = user->next) {
	    if (user->preserved && user->raw[0] != '\0')
		fprintf(file, "%s\n", user->raw);
	    else
		fprintf(file, "%s:x:%u:%u:vperm user:/root:%s\n",
			user->name, (unsigned) user->uid,
			(unsigned) user->gid, user->shell);
	}
    }

    fflush(file);
    fclose(file);
}

/*
 * Resolve a user name from the live /etc/passwd first, then users.conf.
 * Returns false when the user is unknown.
 */
static bool vmap_resolve(VpermConfig *config, const char *name, uid_t *uid,
			 gid_t *gid)
{
    char path[PATH_MAX];
    char line[PATH_MAX];
    FILE *file;
    const VmapUser *user;

    vmap_passwd_path(config, path, sizeof(path));
    file = fopen(path, "r");
    if (file != NULL) {
	while (fgets(line, sizeof(line), file) != NULL) {
	    char entry[64];
	    unsigned entry_uid;
	    unsigned entry_gid;

	    if (sscanf(line, "%63[^:]:%*[^:]:%u:%u", entry, &entry_uid,
		       &entry_gid) != 3)
		continue;
	    if (strcmp(entry, name) == 0) {
		*uid = (uid_t) entry_uid;
		*gid = (gid_t) entry_gid;
		fclose(file);
		return true;
	    }
	}
	fclose(file);
    }

    user = vmap_find(config, name);
    if (user == NULL)
	return false;

    *uid = user->uid;
    *gid = user->gid;
    return true;
}

/* The shim area must survive anything the container does to it.  */
static bool vmap_is_protected(VpermConfig *config, const char *host)
{
    char prefix[PATH_MAX];
    size_t length;

    if (config->map_dir == NULL)
	return false;

    snprintf(prefix, sizeof(prefix), "%s/shim/", config->map_dir);
    length = strlen(prefix);

    return strncmp(host, prefix, length) == 0;
}

static const VmapUser *vmap_find(VpermConfig *config, const char *name)
{
    VmapUser *user;

    for (user = config->users; user != NULL; user = user->next) {
	if (strcmp(user->name, name) == 0)
	    return user;
    }

    return NULL;
}

static const char *const vmap_su_script =
    "#!/bin/sh\n"
    "# Virtual su shim generated by uvroot (vperm).\n"
    "target=root\n"
    "cmd=\n"
    "while [ $# -gt 0 ]; do\n"
    "  case \"$1\" in\n"
    "    -c) cmd=\"$2\"; shift 2 ;;\n"
    "    -l|-i|-) shift ;;\n"
    "    -*) shift ;;\n"
    "    *) target=\"$1\"; shift ;;\n"
    "  esac\n"
    "done\n"
    "if [ -n \"$cmd\" ]; then\n"
    "  exec " VMAP_MAGIC " \"$target\" /bin/sh -c \"$cmd\"\n"
    "fi\n"
    "exec " VMAP_MAGIC " \"$target\" /bin/sh -l\n";

static const char *const vmap_sudo_script =
    "#!/bin/sh\n"
    "# Virtual sudo shim generated by uvroot (vperm).\n"
    "target=root\n"
    "while [ $# -gt 0 ]; do\n"
    "  case \"$1\" in\n"
    "    -u) target=\"$2\"; shift 2 ;;\n"
    "    -E|-H|-i|-s|-) shift ;;\n"
    "    -*) shift ;;\n"
    "    *) break ;;\n"
    "  esac\n"
    "done\n"
    "if [ $# -eq 0 ]; then\n"
    "  echo \"sudo: a command is required\" >&2\n"
    "  exit 1\n"
    "fi\n"
    "prog=$(command -v -- \"$1\") || {\n"
    "  echo \"sudo: $1: command not found\" >&2\n"
    "  exit 1\n"
    "}\n"
    "shift\n"
    "exec " VMAP_MAGIC " \"$target\" \"$prog\" \"$@\"\n";

static void vmap_write_script(const char *path, const char *content)
{
    FILE *file = fopen(path, "w");

    if (file == NULL)
	return;
    fputs(content, file);
    fflush(file);
    fclose(file);
    (void) chmod(path, 0755);
}

static void vmap_expose(Tracee *tracee, const char *source, const char *guest)
{
    char location[PATH_MAX];
    Binding *binding;

    /*
     * Append '!' so uvroot does not dereference the guest location: on
     * Alpine /bin/su is a symlink to /bin/busybox, and dereferencing it
     * would bind the shim over busybox itself.
     */
    snprintf(location, sizeof(location), "%s!", guest);

    binding = new_binding(tracee, source, location, true);
    if (binding == NULL)
	VERBOSE(tracee, 1, "vperm: cannot expose %s as %s", source, guest);
    else
	binding->internal = true;
}

/*
 * Prepare the mapping directory, then bind the generated shims into the
 * container so that su/sudo are the virtual ones.  Idempotent.
 */
static void vmap_setup(Tracee *tracee, VpermConfig *config)
{
    static const char *const su_paths[] = {
	"/usr/local/bin/su", "/bin/su", "/usr/bin/su", NULL
    };
    static const char *const sudo_paths[] = {
	"/usr/local/bin/sudo", "/bin/sudo", "/usr/bin/sudo", NULL
    };
    char shim_dir[PATH_MAX];
    char su[PATH_MAX];
    char sudo[PATH_MAX];
    size_t i;

    if (config->shims_ready)
	return;
    config->shims_ready = true;

    if (config->map_dir == NULL)
	config->map_dir = talloc_strdup(config, vmap_base());
    if (config->map_dir == NULL)
	return;

    if (strlen(config->map_dir) + sizeof("/shim/sudo") >= sizeof(shim_dir))
	return;
    snprintf(shim_dir, sizeof(shim_dir), "%s/shim", config->map_dir);
    if (strlen(shim_dir) + sizeof("/sudo") >= sizeof(su))
	return;
    snprintf(su, sizeof(su), "%s/su", shim_dir);
    snprintf(sudo, sizeof(sudo), "%s/sudo", shim_dir);

    mkdir_recursive(config->map_dir);
    mkdir_recursive(shim_dir);

    {
	char resolved[PATH_MAX];

	if (realpath(config->map_dir, resolved) != NULL) {
	    TALLOC_FREE(config->map_dir);
	    config->map_dir = talloc_strdup(config, resolved);
	    if (config->map_dir == NULL)
		return;
	    snprintf(shim_dir, sizeof(shim_dir), "%s/shim", config->map_dir);
	}
    }

    vmap_load(tracee, config);
    vmap_write_passwd(config);

    /* The virtual /etc/passwd is always exposed: it is what the user
     * mapping is read from and written to by the container.  */
    {
	char passwd[PATH_MAX];

	vmap_passwd_path(config, passwd, sizeof(passwd));
	if (strlen(passwd) + 1 < sizeof(passwd))
	    vmap_expose(tracee, passwd, "/etc/passwd");
    }

    /* --vperm-nosu: no virtual su/sudo, the identity can then only be
     * chosen when the container is started.  */
    if (config->no_shims)
	return;

    vmap_write_script(su, vmap_su_script);
    vmap_write_script(sudo, vmap_sudo_script);

    for (i = 0; su_paths[i] != NULL; i++)
	vmap_expose(tracee, su, su_paths[i]);
    for (i = 0; sudo_paths[i] != NULL; i++)
	vmap_expose(tracee, sudo, sudo_paths[i]);
}

/*
 * Called when the container execs the magic path from a shim:
 *   argv = [magic, target_user, program, args...]
 * The virtual identity is switched to the target user and the program
 * becomes the new image, with argv shifted so the program sees a normal
 * command line.
 */
static int vmap_switch(Tracee *tracee, VpermConfig *config,
		       VpermTracee *state)
{
    word_t argv_address = peek_reg(tracee, ORIGINAL, SYSARG_2);
    size_t word_size = sizeof_word(tracee);
    word_t pointers[64];
    char target[PATH_MAX];
    char program[PATH_MAX];
    int count = 0;
    int i;

    target[0] = '\0';
    program[0] = '\0';

    for (i = 0; i < 64; i++) {
	word_t pointer =
	    peek_word(tracee, argv_address + (word_t) (i * word_size));

	if (errno != 0 || pointer == 0)
	    break;

	pointers[count++] = pointer;

	if (i == 1) {
	    if (read_path(tracee, target, pointer) < 0)
		target[0] = '\0';
	} else if (i == 2) {
	    if (read_path(tracee, program, pointer) < 0)
		program[0] = '\0';
	}
    }

    if (count < 3 || program[0] == '\0') {
	note(tracee, ERROR, USER,
	     "vperm: malformed virtual su/sudo invocation");
	return -EINVAL;
    }

    /* A numeric target is accepted directly, otherwise users.conf is
     * consulted ("root" always resolves).  */
    {
	uid_t caller_uid = state->uid;
	char *end = NULL;
	unsigned long numeric = strtoul(target, &end, 10);

	if (target[0] != '\0' && end != NULL && *end == '\0') {
	    state->uid = (uid_t) numeric;
	    state->gid = (gid_t) numeric;
	} else {
	    uid_t resolved_uid;
	    gid_t resolved_gid;

	    if (!vmap_resolve(config, target, &resolved_uid, &resolved_gid)) {
		note(tracee, ERROR, USER,
		     "vperm: unknown user \"%s\" (see %s/passwd)",
		     target, config->map_dir);
		return -EINVAL;
	    }
	    state->uid = resolved_uid;
	    state->gid = resolved_gid;
	}

	/*
	 * Without the virtual su/sudo shims only the virtual root may
	 * switch identity, and an unprivileged id may never switch to
	 * root -- not even through the shims.
	 */
	if (caller_uid != 0
	    && (config->no_shims || state->uid == 0)) {
	    state->uid = caller_uid;
	    note(tracee, ERROR, USER,
		 "vperm: %s may not switch to \"%s\"",
		 caller_uid == 0 ? "root" : "this id", target);
	    return -EPERM;
	}
    }

    /* argv becomes [program, args...] by dropping the target slot.  */
    for (i = 0; i < count - 2; i++)
	poke_word(tracee, argv_address + (word_t) (i * word_size),
		  pointers[i + 2]);
    poke_word(tracee, argv_address + (word_t) ((count - 2) * word_size), 0);

    if (set_sysarg_path(tracee, program, SYSARG_1) < 0)
	return -EFAULT;

    VERBOSE(tracee, 1, "vperm: virtual switch to %s (%u:%u), exec %s",
	    target, (unsigned) state->uid, (unsigned) state->gid, program);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Roots                                                               */
/* ------------------------------------------------------------------ */

static unsigned int hash_path(const char *path)
{
    unsigned int hash = 2166136261u;

    while (*path != '\0') {
	hash ^= (unsigned char) *path++;
	hash *= 16777619u;
    }

    return hash;
}

static void mkdir_recursive(const char *path)
{
    char buffer[PATH_MAX];
    char *cursor;

    snprintf(buffer, sizeof(buffer), "%s", path);
    for (cursor = buffer + 1; *cursor != '\0'; cursor++) {
	if (*cursor != '/')
	    continue;
	*cursor = '\0';
	(void) mkdir(buffer, 0700);
	*cursor = '/';
    }
    (void) mkdir(buffer, 0700);
}

static char *default_db_path(Tracee *tracee, VpermConfig *config,
			     const char *host)
{
    if (config->explicit_db != NULL && config->roots == NULL)
	return talloc_strdup(config, config->explicit_db);

    /* A netfs root lives in a temporary cache: the database must not be
     * part of it, otherwise it would be pushed to the server.  */
    if (netfs_mount_guest_for_host(tracee, host) != NULL) {
	const char *base = getenv("XDG_DATA_HOME");
	char directory[PATH_MAX];

	if (base == NULL || base[0] == '\0') {
	    const char *home = getenv("HOME");

	    if (home == NULL || home[0] == '\0')
		home = "/tmp";
	    snprintf(directory, sizeof(directory), "%s/.local/share/uvroot/vperm",
		     home);
	} else
	    snprintf(directory, sizeof(directory), "%s/uvroot/vperm", base);

	mkdir_recursive(directory);

	return talloc_asprintf(config, "%s/%08x.db", directory,
			       hash_path(host));
    }

    return talloc_asprintf(config, "%s/%s", host, VPERM_DB_NAME);
}

static void root_db_rel(VpermRoot *root)
{
    size_t length = strlen(root->host);

    if (strncmp(root->db, root->host, length) == 0
	&& root->db[length] == '/') {
	root->db_rel = talloc_strdup(root, root->db + length + 1);
    } else
	root->db_rel = NULL;
}

/*
 * Remember the database's identity so that names other than the database
 * path (hard links) can be recognized and refused as well.
 */
static void root_db_ident(VpermRoot *root)
{
    struct stat st;

    root->db_id = false;
    if (root->db != NULL && stat(root->db, &st) == 0) {
	root->db_dev = st.st_dev;
	root->db_ino = st.st_ino;
	root->db_id = true;
    }
}

static int root_add(Tracee *tracee, VpermConfig *config, const char *host,
		    const char *guest)
{
    VpermRoot *root;
    char resolved[PATH_MAX];
    const char *canonical = host;

    if (host == NULL || host[0] == '\0')
	return 0;

    if (realpath(host, resolved) != NULL)
	canonical = resolved;

    for (root = config->roots; root != NULL; root = root->next) {
	if (strcmp(root->host, canonical) == 0)
	    return 0;
    }

    root = talloc_zero(config, VpermRoot);
    if (root == NULL)
	return -ENOMEM;

    root->host = talloc_strdup(root, canonical);
    root->guest = talloc_strdup(root, guest != NULL ? guest : "/");
    root->local = (netfs_mount_guest_for_host(tracee, canonical) == NULL);
    /*
     * A block-backed root (image/NBD/iSCSI) stores ownership and mode in
     * its own filesystem: the virtual database must not shadow it, so no
     * entry is ever recorded or applied for those paths.
     */
    root->fs_backed = netfs_is_block_root(tracee, canonical);

    {
	char *db = default_db_path(tracee, config, canonical);

	if (db == NULL) {
	    talloc_free(root);
	    return -ENOMEM;
	}
	root->db = talloc_steal(root, db);
    }

    /*
     * Take over a database left behind under one of the old names.
     * When the current name is absent and a legacy one exists, move it
     * so that the recorded virtual ownership is not silently abandoned.
     * If the move fails (read-only root) the legacy file is used in
     * place.
     */
    if (root->local && !root->fs_backed) {
	struct stat st;

	if (stat(root->db, &st) != 0 && errno == ENOENT) {
	    const char *const *legacy;

	    for (legacy = vperm_db_legacy; *legacy != NULL; legacy++) {
		char old[PATH_MAX];

		if ((size_t) snprintf(old, sizeof(old), "%s/%s",
				     canonical, *legacy) >= sizeof(old)
		    || stat(old, &st) != 0)
		    continue;

		if (rename(old, root->db) != 0) {
		    char *kept = talloc_strdup(root, old);

		    if (kept != NULL) {
			note(tracee, WARNING, USER,
			     "vperm: cannot migrate %s: %s (used in place)",
			     old, strerror(errno));
			talloc_free(root->db);
			root->db = kept;
		    }
		}
		break;
	    }
	}
    }

    root_db_rel(root);
    root_db_ident(root);

    if (db_load(tracee, root) < 0) {
	talloc_free(root);
	return -EINVAL;
    }

    root->next = config->roots;
    config->roots = root;
    return 0;
}

static int roots_build(Tracee *tracee, VpermConfig *config)
{
    const char *root_host;
    const char *guest;
    const char *cache;
    unsigned int i;
    int status;

    if (config->built)
	return config->build_status;
    config->built = true;
    config->build_status = 0;

    root_host = get_root(tracee);
    if (root_host != NULL) {
	status = root_add(tracee, config, root_host, "/");
	if (status < 0)
	    goto fail;
    }

    for (i = 0; netfs_mount_at(tracee, i, &guest, &cache); i++) {
	status = root_add(tracee, config, cache, guest);
	if (status < 0)
	    goto fail;
    }

    return 0;

fail:
    config->build_status = status;
    return status;
}

static VpermRoot *find_root(VpermConfig *config, const char *host,
			    const char **rel)
{
    VpermRoot *root;
    VpermRoot *best = NULL;
    size_t best_length = 0;

    for (root = config->roots; root != NULL; root = root->next) {
	size_t length = strlen(root->host);

	if (strncmp(host, root->host, length) != 0)
	    continue;
	if (host[length] != '\0' && host[length] != '/')
	    continue;
	if (length < best_length)
	    continue;

	best = root;
	best_length = length;
    }

    if (best == NULL)
	return NULL;

    *rel = (host[best_length] == '/') ? host + best_length + 1
	: host + best_length;

    return best;
}

/* ------------------------------------------------------------------ */
/* Per-tracee state                                                    */
/* ------------------------------------------------------------------ */

static VpermTracee *find_tracee_state(VpermConfig *config, pid_t pid)
{
    VpermTracee *state;

    for (state = config->tracees; state != NULL; state = state->next) {
	if (state->pid == pid)
	    return state;
    }

    return NULL;
}

static VpermTracee *get_tracee_state(VpermConfig *config, pid_t pid,
				     bool create)
{
    VpermTracee *state = find_tracee_state(config, pid);

    if (state != NULL || !create)
	return state;

    state = talloc_zero(config, VpermTracee);
    if (state == NULL)
	return NULL;

    state->pid = pid;
    state->uid = config->uid;
    state->gid = config->gid;

    state->next = config->tracees;
    config->tracees = state;
    return state;
}

static VpermFd *fd_find(VpermTracee *state, int fd)
{
    VpermFd *item;

    for (item = state->fds; item != NULL; item = item->next) {
	if (item->fd == fd)
	    return item;
    }

    return NULL;
}

static void fd_add(VpermTracee *state, int fd, VpermRoot *root,
		   const char *rel)
{
    VpermFd *item;

    if (fd < 0 || root == NULL)
	return;

    item = fd_find(state, fd);
    if (item == NULL) {
	item = talloc_zero(state, VpermFd);
	if (item == NULL)
	    return;
	item->fd = fd;
	item->next = state->fds;
	state->fds = item;
    }

    item->root = root;
    snprintf(item->rel, sizeof(item->rel), "%s", rel);
}

static void fd_remove(VpermTracee *state, int fd)
{
    VpermFd **link;

    for (link = &state->fds; *link != NULL; link = &(*link)->next) {
	if ((*link)->fd == fd) {
	    VpermFd *item = *link;
	    *link = item->next;
	    talloc_free(item);
	    return;
	}
    }
}

static void fd_duplicate(VpermTracee *state, int old_fd, int new_fd)
{
    VpermFd *item;

    if (old_fd < 0 || new_fd < 0 || old_fd == new_fd)
	return;

    item = fd_find(state, old_fd);
    if (item != NULL)
	fd_add(state, new_fd, item->root, item->rel);
}

/* ------------------------------------------------------------------ */
/* Permission model                                                    */
/* ------------------------------------------------------------------ */

static const char *relative_of(VpermRoot *root, const char *host,
			       const char *rel)
{
    struct stat st;

    if (root->db_rel != NULL && strcmp(rel, root->db_rel) == 0)
	return NULL;

    /*
     * Same file, another name: a hard link to the database must be
     * refused too, otherwise the container could read and rewrite the
     * database through it.  lstat(2) is deliberate -- what matters is
     * the inode of the name itself, so that creating a symlink *to* the
     * database stays allowed (access through it is caught above, since
     * the path is canonicalized before it gets here).
     */
    if (root->db_id && host != NULL
	&& lstat(host, &st) == 0
	&& st.st_dev == root->db_dev && st.st_ino == root->db_ino)
	return NULL;

    return rel;
}

static bool perm_allows(const VpermEntry *entry, uid_t uid, gid_t gid,
			int want)
{
    mode_t bits;

    if (uid == 0) {
	/* Root may do anything but execute a file with no x bit.  */
	if ((want & X_OK) != 0 && (entry->mode & 0111) == 0)
	    return false;
	return true;
    }

    if (uid == entry->uid)
	bits = (entry->mode >> 6) & 7;
    else if (gid == entry->gid)
	bits = (entry->mode >> 3) & 7;
    else
	bits = entry->mode & 7;

    if ((want & R_OK) != 0 && (bits & 4) == 0)
	return false;
    if ((want & W_OK) != 0 && (bits & 2) == 0)
	return false;
    if ((want & X_OK) != 0 && (bits & 1) == 0)
	return false;

    return true;
}

static void remember_change(VpermRoot *root, const char *rel, mode_t mode,
			    bool have_mode, uid_t uid, bool have_uid,
			    gid_t gid, bool have_gid)
{
    bool fresh = (entry_find(root, rel) == NULL);
    VpermEntry *entry = entry_get(root, rel, true);

    if (entry == NULL)
	return;

    /*
     * A brand new entry inherits whatever the caller did not specify
     * from the host, so that a chmod keeps the current owner and a
     * chown keeps the current mode.  Existing entries are never
     * silently re-read from the host.
     */
    if (fresh && (!have_mode || !have_uid || !have_gid)) {
	char full[PATH_MAX];
	struct stat st;

	snprintf(full, sizeof(full), "%s/%s", root->host, rel);
	if (stat(full, &st) == 0) {
	    if (!have_mode)
		entry->mode = st.st_mode;
	    if (!have_uid)
		entry->uid = st.st_uid;
	    if (!have_gid)
		entry->gid = st.st_gid;
	}
    }

    if (have_mode)
	entry->mode = (entry->mode & S_IFMT) | (mode & 07777);

    /* A freshly created entry has no type yet: default to a file.  */
    if ((entry->mode & S_IFMT) == 0)
	entry->mode |= S_IFREG;

    if (have_uid)
	entry->uid = uid;
    if (have_gid)
	entry->gid = gid;

    (void) db_save(root);
}

/*
 * A path was removed but its entry was kept, and is now being created
 * again (symlink, hard link): the recorded virtual owner and permission
 * bits stay, only the object type has to follow what was created.
 * @type is 0 to take it from the object at @host.
 */
static void refresh_entry_type(VpermTracee *state, mode_t type,
			       const char *host)
{
    VpermEntry *entry = entry_find(state->pend_root, state->pend_rel);

    if (entry == NULL)
	return;

    if (type == 0 && host != NULL) {
	struct stat st;

	if (lstat(host, &st) == 0)
	    type = st.st_mode & S_IFMT;
    }

    if (type != 0)
	entry->mode = (entry->mode & 07777) | type;

    (void) db_save(state->pend_root);
}

/* ------------------------------------------------------------------ */
/* Syscall hooks                                                       */
/* ------------------------------------------------------------------ */

static int handle_enter_start(Tracee *tracee, VpermConfig *config)
{
    VpermTracee *state = get_tracee_state(config, tracee->pid, true);
    Sysnum sysnum = get_sysnum(tracee, ORIGINAL);
    VpermFd *item;

    if (state == NULL)
	return 0;

    state->pend_valid = false;
    state->pend_absent = false;
    state->pend_new_valid = false;
    state->move_valid = false;
    state->last_valid = false;
    state->pend_protected = false;
    state->pend_fs_backed = false;
    state->emulate = false;

    if (sysnum == PR_execve) {
	char guest_path[PATH_MAX];

	if (get_sysarg_path(tracee, guest_path, SYSARG_1) >= 0
	    && strcmp(guest_path, VMAP_MAGIC) == 0)
	    return vmap_switch(tracee, config, state);
    }

    switch (sysnum) {
    case PR_close:{
	int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);

	fd_remove(state, fd);
	break;
    }

    case PR_dup2:
    case PR_dup3:
	fd_remove(state, (int) peek_reg(tracee, CURRENT, SYSARG_2));
	break;

    case PR_fsync:
    case PR_fdatasync:
	item = fd_find(state, (int) peek_reg(tracee, CURRENT, SYSARG_1));
	(void) item;
	break;

    default:
	break;
    }

    return 0;
}

/*
 * Compute the relative path for the syscall's path argument and remember
 * it for the exit stage.  Returns 0 when the path is not covered.
 */
static int capture_path(Tracee *tracee, VpermConfig *config,
			VpermTracee *state, Reg reg)
{
    char host[PATH_MAX];
    const char *rel;
    VpermRoot *root;

    if (get_sysarg_path(tracee, host, reg) < 0)
	return 0;

    /* The mapping area lives outside the guest roots (it is bound in),
     * so it has to be recognized before the root lookup.  */
    state->pend_protected = vmap_is_protected(config, host);

    if (roots_build(tracee, config) < 0)
	return -EINVAL;

    root = find_root(config, host, &rel);
    if (root == NULL)
	return 0;

    snprintf(state->pend_host, sizeof(state->pend_host), "%s", host);
    state->pend_fs_backed = root->fs_backed;
    state->pend_fs_root = root->fs_backed
	&& root->guest != NULL && strcmp(root->guest, "/") == 0;

    /* The filesystem inside the disk is authoritative there.  */
    if (root->fs_backed) {
	/* Entries created by the container inherit its virtual
	 * ownership, which is written into the image.  */
	netfs_block_set_identity(tracee, state->uid, state->gid);
	return 0;
    }

    /* Creation handling needs to know whether this path already
     * existed, so that a stale entry can be refreshed instead of being
     * overwritten by the creator's identity.  */
    {
	struct stat st;

	state->pend_absent = (lstat(host, &st) != 0 && errno == ENOENT);
    }

    if (relative_of(root, host, rel) == NULL)
	return -EACCES;

    state->pend_root = root;
    snprintf(state->pend_rel, sizeof(state->pend_rel), "%s", rel);

    /* Normalize the key: a trailing slash (mkdir -p, open("dir/")) must
     * not create a distinct entry from the same path without it.  */
    {
	size_t length = strlen(state->pend_rel);

	while (length > 0 && state->pend_rel[length - 1] == '/')
	    state->pend_rel[--length] = '\0';
    }

    state->pend_valid = true;
    return 0;
}

static int capture_fd(Tracee *tracee, VpermConfig *config,
		      VpermTracee *state, Reg reg)
{
    VpermFd *item;
    int fd;

    (void) tracee;
    (void) config;

    fd = (int) peek_reg(tracee, CURRENT, reg);
    item = fd_find(state, fd);
    if (item == NULL)
	return 0;

    state->pend_root = item->root;
    snprintf(state->pend_rel, sizeof(state->pend_rel), "%s", item->rel);
    state->pend_valid = true;
    state->pend_fs_backed = item->root->fs_backed;
    state->pend_fs_root = item->root->fs_backed
	&& item->root->guest != NULL && strcmp(item->root->guest, "/") == 0;
    if (item->root->fs_backed) {
	size_t host_length = strlen(item->root->host);
	size_t rel_length = strlen(item->rel);

	if (host_length + 1 + rel_length < sizeof(state->pend_host)) {
	    memcpy(state->pend_host, item->root->host, host_length);
	    state->pend_host[host_length] = '/';
	    memcpy(state->pend_host + host_length + 1, item->rel,
		   rel_length + 1);
	}
    }
    return 0;
}

static int check_access(VpermTracee *state, VpermConfig *config UNUSED, int want)
{
    VpermEntry *entry;

    if (!state->pend_valid || state->pend_root == NULL)
	return 0;

    {
	int status = check_ancestors(state->pend_root, state->pend_rel,
				     state->uid, state->gid);

	if (status < 0)
	    return status;
    }

    entry = entry_find(state->pend_root, state->pend_rel);
    if (entry == NULL)
	return 0;		/* No entry: pass through to the host.  */

    /*
     * The path does not exist (yet).  A stale entry kept from a
     * previous life must not veto the creation: that is governed by the
     * parent directory, and the entry is refreshed once the object is
     * created again.  For a plain open/access the host then reports
     * ENOENT, which is the expected answer.
     */
    if (state->pend_absent)
	return 0;

    if (perm_allows(entry, state->uid, state->gid, want))
	return 0;

    return -EACCES;
}

/*
 * Permission checks against the metadata stored inside a block device:
 * the image, not the host kernel, decides who may do what.  The host
 * kernel cannot do it here because every guest identity is the same host
 * user (the cache files are all owned by it), so a file created by the
 * virtual root would otherwise be readable, writable and removable by
 * every other virtual identity.
 */
static int vperm_mode_check(const struct stat *st, uid_t uid, gid_t gid,
			    int want)
{
    mode_t permissions = st->st_mode;
    mode_t granted;

    /* Root may do anything, except execute a file without any x bit.  */
    if (uid == 0) {
	if ((want & X_OK) != 0 && !S_ISDIR(permissions)
	    && (permissions & 0111) == 0)
	    return -EACCES;
	return 0;
    }

    if (uid == st->st_uid)
	granted = (permissions >> 6) & 07;
    else if (gid == st->st_gid)
	granted = (permissions >> 3) & 07;
    else
	granted = permissions & 07;

    if ((want & R_OK) != 0 && (granted & 04) == 0)
	return -EACCES;
    if ((want & W_OK) != 0 && (granted & 02) == 0)
	return -EACCES;
    if ((want & X_OK) != 0 && (granted & 01) == 0)
	return -EACCES;

    return 0;
}

/*
 * Every ancestor directory inside the image has to be searchable.  The
 * walk stops as soon as netfs no longer knows the path, which is exactly
 * where the mounted image ends (its host directory is not part of it).
 */
static int vperm_fs_ancestors(Tracee *tracee, VpermTracee *state,
			      const char *path)
{
    char buffer[PATH_MAX];
    struct stat st;

    if (path == NULL || strlen(path) >= sizeof(buffer))
	return 0;
    strcpy(buffer, path);

    for (;;) {
	char *slash = strrchr(buffer, '/');

	if (slash == NULL || slash == buffer)
	    break;
	*slash = '\0';

	if (netfs_block_stat(tracee, buffer, &st) < 0)
	    break;

	if (vperm_mode_check(&st, state->uid, state->gid, X_OK) < 0)
	    return -EACCES;
    }

    return 0;
}

/* The parent directory must grant @want (usually W_OK | X_OK).  */
static int vperm_fs_parent(Tracee *tracee, VpermTracee *state,
			   const char *path, int want)
{
    char parent[PATH_MAX];
    struct stat st;
    char *slash;
    int verdict;

    if (path == NULL || strlen(path) >= sizeof(parent))
	return 0;
    strcpy(parent, path);

    slash = strrchr(parent, '/');
    if (slash == NULL || slash == parent)
	return 0;		/* no parent inside the image */
    *slash = '\0';

    verdict = vperm_fs_ancestors(tracee, state, parent);
    if (verdict < 0)
	return verdict;

    if (netfs_block_stat(tracee, parent, &st) < 0)
	return 0;
    if (!S_ISDIR(st.st_mode))
	return -ENOTDIR;

    return vperm_mode_check(&st, state->uid, state->gid, want);
}

/*
 * Check access to the pending path.  When @create is set and the target
 * does not exist yet, the parent directory is checked instead.
 */
static int vperm_fs_access(Tracee *tracee, VpermTracee *state, int want,
			   bool create)
{
    struct stat st;
    int verdict;

    if (!state->pend_fs_backed || state->pend_host[0] == '\0')
	return 0;

    verdict = vperm_fs_ancestors(tracee, state, state->pend_host);
    if (verdict < 0)
	return verdict;

    if (netfs_block_stat(tracee, state->pend_host, &st) < 0) {
	if (create && state->pend_fs_root)
	    return vperm_fs_parent(tracee, state, state->pend_host,
				   W_OK | X_OK);
	return 0;		/* Unknown: let the host decide.  */
    }

    return vperm_mode_check(&st, state->uid, state->gid, want);
}

/* Parent directory of the pending path (unlink, mkdir, rename, ...).  */
static int vperm_fs_check_parent(Tracee *tracee, VpermTracee *state, int want)
{
    if (!state->pend_fs_root || state->pend_host[0] == '\0')
	return 0;

    return vperm_fs_parent(tracee, state, state->pend_host, want);
}

/*
 * chmod(2) is allowed to the owner (and to the virtual root); chown(2)
 * only to the virtual root, except for a no-op or a group change by the
 * owner.
 */
static int vperm_fs_check_owner(Tracee *tracee, VpermTracee *state)
{
    struct stat st;

    if (!state->pend_fs_root || state->pend_host[0] == '\0')
	return 0;
    if (state->uid == 0)
	return 0;
    if (netfs_block_stat(tracee, state->pend_host, &st) < 0)
	return 0;
    if (state->uid == st.st_uid)
	return 0;

    return -EPERM;
}

static int vperm_fs_check_chown(Tracee *tracee, VpermTracee *state, uid_t uid,
				gid_t gid UNUSED)
{
    struct stat st;

    if (!state->pend_fs_root || state->pend_host[0] == '\0')
	return 0;
    if (state->uid == 0)
	return 0;
    if (netfs_block_stat(tracee, state->pend_host, &st) < 0)
	return 0;
    if (state->uid != st.st_uid)
	return -EPERM;

    /* An owner may change the group only, and not the owner.  */
    if (uid != (uid_t) -1 && uid != st.st_uid)
	return -EPERM;

    return 0;
}

/* Returns 1 when the change was delegated to the filesystem, 0 when
 * there is nothing to delegate, or a negative errno.  */
static int vperm_delegate_chmod(Tracee *tracee, VpermTracee *state,
				mode_t mode)
{
    if (!state->pend_fs_backed || state->pend_host[0] == '\0')
	return 0;
    if (netfs_block_chmod(tracee, state->pend_host, mode) < 0)
	return -EROFS;
    return 1;
}

static int vperm_delegate_chown(Tracee *tracee, VpermTracee *state, uid_t uid,
				gid_t gid)
{
    if (!state->pend_fs_backed || state->pend_host[0] == '\0')
	return 0;
    if (netfs_block_chown(tracee, state->pend_host, uid, gid) < 0)
	return -EROFS;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Virtual process permissions                                         */
/* ------------------------------------------------------------------ */

/*
 * A process may only control another process that belongs to the same
 * virtual identity; the virtual root may control everything.  The host
 * kernel cannot make that decision because every guest process really
 * runs as the same host user.
 */
static bool vperm_same_identity(VpermConfig *config, VpermTracee *state,
				pid_t pid)
{
    VpermTracee *other;

    if (state->uid == 0)
	return true;

    other = find_tracee_state(config, pid);
    if (other == NULL)
	return false;		/* not a container process */

    return other->uid == state->uid && other->gid == state->gid;
}

static bool vperm_group_allowed(VpermConfig *config, VpermTracee *state,
				pid_t pgid)
{
    VpermTracee *other;

    for (other = config->tracees; other != NULL; other = other->next) {
	if (getpgid(other->pid) != pgid)
	    continue;
	if (other->uid != state->uid || other->gid != state->gid)
	    return false;
    }

    return true;
}

static bool vperm_all_allowed(VpermConfig *config, VpermTracee *state)
{
    VpermTracee *other;

    for (other = config->tracees; other != NULL; other = other->next) {
	if (other->uid != state->uid || other->gid != state->gid)
	    return false;
    }

    return true;
}

/* Check a target pid as passed to kill(2).  */
static int vperm_check_signal(Tracee *tracee, VpermConfig *config,
			      VpermTracee *state, pid_t pid)
{
    if (state->uid == 0)
	return 0;

    if (pid > 0)
	return vperm_same_identity(config, state, pid) ? 0 : -EPERM;

    if (pid == 0)
	return vperm_group_allowed(config, state, getpgid(tracee->pid))
	    ? 0 : -EPERM;

    if (pid == -1)
	return vperm_all_allowed(config, state) ? 0 : -EPERM;

    return vperm_group_allowed(config, state, -pid) ? 0 : -EPERM;
}

/* Resolve the pid a pidfd refers to, for pidfd_send_signal(2).  */
static pid_t vperm_pidfd_target(Tracee *tracee, int pidfd)
{
    char path[64];
    char line[128];
    FILE *file;
    pid_t pid = -1;

    snprintf(path, sizeof(path), "/proc/%d/fdinfo/%d", tracee->pid, pidfd);
    file = fopen(path, "r");
    if (file == NULL)
	return -1;

    while (fgets(line, sizeof(line), file) != NULL) {
	if (strncmp(line, "Pid:", 4) == 0) {
	    pid = (pid_t) strtol(line + 4, NULL, 10);
	    break;
	}
    }
    fclose(file);
    return pid;
}

static int handle_enter_end(Tracee *tracee, VpermConfig *config,
			    intptr_t status)
{
    VpermTracee *state = get_tracee_state(config, tracee->pid, true);
    Sysnum sysnum = get_sysnum(tracee, ORIGINAL);

    if (state == NULL || status < 0)
	return 0;

    switch (sysnum) {
    /* Metadata: never touch the host, record in the database.  */
    case PR_chmod:
	if (capture_path(tracee, config, state, SYSARG_1) < 0)
	    return -EACCES;
	if (vperm_fs_check_owner(tracee, state) < 0)
	    return -EPERM;
	if (vperm_delegate_chmod(tracee, state,
				 peek_reg(tracee, ORIGINAL, SYSARG_2)) > 0) {
	    set_sysnum(tracee, PR_void);
	    state->emulate = true;
	    break;
	}
	if (state->pend_protected)
	    return -EPERM;
	if (state->pend_valid) {
	    remember_change(state->pend_root, state->pend_rel,
			    peek_reg(tracee, ORIGINAL, SYSARG_2), true,
			    0, false, 0, false);
	    set_sysnum(tracee, PR_void);
	    state->emulate = true;
	}
	break;

    case PR_fchmod:
	capture_fd(tracee, config, state, SYSARG_1);
	if (vperm_fs_check_owner(tracee, state) < 0)
	    return -EPERM;
	if (vperm_delegate_chmod(tracee, state,
				 peek_reg(tracee, ORIGINAL, SYSARG_2)) > 0) {
	    set_sysnum(tracee, PR_void);
	    state->emulate = true;
	    break;
	}
	if (state->pend_protected)
	    return -EPERM;
	if (state->pend_valid) {
	    remember_change(state->pend_root, state->pend_rel,
			    peek_reg(tracee, ORIGINAL, SYSARG_2), true,
			    0, false, 0, false);
	    set_sysnum(tracee, PR_void);
	    state->emulate = true;
	}
	break;

    case PR_fchmodat:
    case PR_fchmodat2:
	if (capture_path(tracee, config, state, SYSARG_2) < 0)
	    return -EACCES;
	if (vperm_fs_check_owner(tracee, state) < 0)
	    return -EPERM;
	if (vperm_delegate_chmod(tracee, state,
				 peek_reg(tracee, ORIGINAL, SYSARG_3)) > 0) {
	    set_sysnum(tracee, PR_void);
	    state->emulate = true;
	    break;
	}
	if (state->pend_protected)
	    return -EPERM;
	if (state->pend_valid) {
	    remember_change(state->pend_root, state->pend_rel,
			    peek_reg(tracee, ORIGINAL, SYSARG_3), true,
			    0, false, 0, false);
	    set_sysnum(tracee, PR_void);
	    state->emulate = true;
	}
	break;

    case PR_chown:
    case PR_chown32:
    case PR_lchown:
    case PR_lchown32:
	if (capture_path(tracee, config, state, SYSARG_1) < 0)
	    return -EACCES;
	if (vperm_fs_check_chown(tracee, state,
				 (uid_t) peek_reg(tracee, ORIGINAL, SYSARG_2),
				 (gid_t) peek_reg(tracee, ORIGINAL,
						  SYSARG_3)) < 0)
	    return -EPERM;
	if (vperm_delegate_chown(tracee, state,
				 (uid_t) peek_reg(tracee, ORIGINAL, SYSARG_2),
				 (gid_t) peek_reg(tracee, ORIGINAL,
						  SYSARG_3)) > 0) {
	    set_sysnum(tracee, PR_void);
	    state->emulate = true;
	    break;
	}
	if (state->pend_protected)
	    return -EPERM;
	if (state->pend_valid) {
	    remember_change(state->pend_root, state->pend_rel, 0, false,
			    peek_reg(tracee, ORIGINAL, SYSARG_2), true,
			    peek_reg(tracee, ORIGINAL, SYSARG_3), true);
	    set_sysnum(tracee, PR_void);
	    state->emulate = true;
	}
	break;

    case PR_fchown:
    case PR_fchown32:
	capture_fd(tracee, config, state, SYSARG_1);
	if (vperm_fs_check_chown(tracee, state,
				 (uid_t) peek_reg(tracee, ORIGINAL, SYSARG_2),
				 (gid_t) peek_reg(tracee, ORIGINAL,
						  SYSARG_3)) < 0)
	    return -EPERM;
	if (vperm_delegate_chown(tracee, state,
				 (uid_t) peek_reg(tracee, ORIGINAL, SYSARG_2),
				 (gid_t) peek_reg(tracee, ORIGINAL,
						  SYSARG_3)) > 0) {
	    set_sysnum(tracee, PR_void);
	    state->emulate = true;
	    break;
	}
	if (state->pend_protected)
	    return -EPERM;
	if (state->pend_valid) {
	    remember_change(state->pend_root, state->pend_rel, 0, false,
			    peek_reg(tracee, ORIGINAL, SYSARG_2), true,
			    peek_reg(tracee, ORIGINAL, SYSARG_3), true);
	    set_sysnum(tracee, PR_void);
	    state->emulate = true;
	}
	break;

    case PR_fchownat:
	if (capture_path(tracee, config, state, SYSARG_2) < 0)
	    return -EACCES;
	if (vperm_fs_check_chown(tracee, state,
				 (uid_t) peek_reg(tracee, ORIGINAL, SYSARG_3),
				 (gid_t) peek_reg(tracee, ORIGINAL,
						  SYSARG_4)) < 0)
	    return -EPERM;
	if (vperm_delegate_chown(tracee, state,
				 (uid_t) peek_reg(tracee, ORIGINAL, SYSARG_3),
				 (gid_t) peek_reg(tracee, ORIGINAL,
						  SYSARG_4)) > 0) {
	    set_sysnum(tracee, PR_void);
	    state->emulate = true;
	    break;
	}
	if (state->pend_protected)
	    return -EPERM;
	if (state->pend_valid) {
	    remember_change(state->pend_root, state->pend_rel, 0, false,
			    peek_reg(tracee, ORIGINAL, SYSARG_3), true,
			    peek_reg(tracee, ORIGINAL, SYSARG_4), true);
	    set_sysnum(tracee, PR_void);
	    state->emulate = true;
	}
	break;

    /* Statistics: remember which path was queried.  */
    case PR_stat:
    case PR_stat64:
    case PR_lstat:
    case PR_lstat64:
    case PR_oldstat:
    case PR_oldlstat:
	capture_path(tracee, config, state, SYSARG_1);
	break;

    case PR_newfstatat:
    case PR_fstatat64:
    case PR_statx:
	capture_path(tracee, config, state, SYSARG_2);
	break;

    case PR_fstat:
    case PR_fstat64:
	capture_fd(tracee, config, state, SYSARG_1);
	break;

    /* Access control.  */
    case PR_open:
    case PR_creat:{
	word_t flags = (sysnum == PR_open)
	    ? peek_reg(tracee, ORIGINAL, SYSARG_2)
	    : (O_CREAT | O_WRONLY | O_TRUNC);
	int want = 0;

	if (capture_path(tracee, config, state, SYSARG_1) < 0)
	    return -EACCES;

	if ((flags & O_ACCMODE) == O_RDONLY)
	    want = R_OK;
	else if ((flags & O_ACCMODE) == O_WRONLY)
	    want = W_OK;
	else
	    want = R_OK | W_OK;

	if (state->pend_protected && (flags & O_ACCMODE) != O_RDONLY)
	    return -EPERM;

	{
	    int verdict = check_access(state, config, want);

	    if (verdict == 0)
		verdict = vperm_fs_access(tracee, state, want,
					  (flags & O_CREAT) != 0);
	    if (verdict < 0)
		return verdict;
	}
	break;
    }

    case PR_openat:{
	word_t flags = peek_reg(tracee, ORIGINAL, SYSARG_3);
	int want;

	if ((flags & O_ACCMODE) == O_RDONLY)
	    want = R_OK;
	else if ((flags & O_ACCMODE) == O_WRONLY)
	    want = W_OK;
	else
	    want = R_OK | W_OK;

	if (capture_path(tracee, config, state, SYSARG_2) < 0)
	    return -EACCES;

	if (state->pend_protected && (flags & O_ACCMODE) != O_RDONLY)
	    return -EPERM;

	{
	    int verdict = check_access(state, config, want);

	    if (verdict == 0)
		verdict = vperm_fs_access(tracee, state, want,
					  (flags & O_CREAT) != 0);
	    if (verdict < 0)
		return verdict;
	}
	break;
    }

    case PR_access:
	if (capture_path(tracee, config, state, SYSARG_1) < 0)
	    return -EACCES;
	{
	    int want = (int) peek_reg(tracee, ORIGINAL, SYSARG_2);
	    int verdict = check_access(state, config, want);

	    if (verdict == 0)
		verdict = vperm_fs_access(tracee, state, want, false);
	    if (verdict < 0)
		return verdict;
	}
	break;

    case PR_faccessat:
    case PR_faccessat2:
	if (capture_path(tracee, config, state, SYSARG_2) < 0)
	    return -EACCES;
	{
	    int want = (int) peek_reg(tracee, ORIGINAL, SYSARG_3);
	    int verdict = check_access(state, config, want);

	    if (verdict == 0)
		verdict = vperm_fs_access(tracee, state, want, false);
	    if (verdict < 0)
		return verdict;
	}
	break;

    case PR_execve:{
	int verdict;

	if (capture_path(tracee, config, state, SYSARG_1) < 0)
	    return -EACCES;
	verdict = check_access(state, config, X_OK);
	if (verdict == 0)
	    verdict = vperm_fs_access(tracee, state, X_OK, false);
	if (verdict < 0)
	    return verdict;
	break;
    }

    /* The mapping area (shims) is read-only for the container.  */
    case PR_unlink:
    case PR_rmdir:
    case PR_mkdir:
	if (capture_path(tracee, config, state, SYSARG_1) < 0)
	    return -EACCES;
	if (state->pend_protected)
	    return -EPERM;
	if (state->pend_valid) {
	    int verdict = check_parent_write(state->pend_root,
					     state->pend_rel,
					     state->uid, state->gid);

	    if (verdict < 0)
		return verdict;
	} else {
	    int verdict = vperm_fs_check_parent(tracee, state,
						W_OK | X_OK);

	    if (verdict < 0)
		return verdict;
	}
	break;

    case PR_unlinkat:
    case PR_mkdirat:
	if (capture_path(tracee, config, state, SYSARG_2) < 0)
	    return -EACCES;
	if (state->pend_protected)
	    return -EPERM;
	if (state->pend_valid) {
	    int verdict = check_parent_write(state->pend_root,
					     state->pend_rel,
					     state->uid, state->gid);

	    if (verdict < 0)
		return verdict;
	} else {
	    int verdict = vperm_fs_check_parent(tracee, state,
						W_OK | X_OK);

	    if (verdict < 0)
		return verdict;
	}
	break;

    /*
     * Block-backed roots: truncating and link creation are decided by
     * the image metadata too, not by the host cache files.
     */
    case PR_truncate:
    case PR_truncate64:
	if (capture_path(tracee, config, state, SYSARG_1) < 0)
	    return -EACCES;
	if (!state->pend_valid) {
	    int verdict = vperm_fs_access(tracee, state, W_OK, false);

	    if (verdict < 0)
		return verdict;
	}
	break;

    case PR_ftruncate:
    case PR_ftruncate64:
	capture_fd(tracee, config, state, SYSARG_1);
	if (state->pend_fs_backed) {
	    int verdict = vperm_fs_access(tracee, state, W_OK, false);

	    if (verdict < 0)
		return verdict;
	}
	break;

    case PR_symlink:
	if (capture_path(tracee, config, state, SYSARG_2) < 0)
	    return -EACCES;
	if (!state->pend_valid) {
	    int verdict = vperm_fs_check_parent(tracee, state,
						W_OK | X_OK);

	    if (verdict < 0)
		return verdict;
	}
	break;

    case PR_symlinkat:
	if (capture_path(tracee, config, state, SYSARG_3) < 0)
	    return -EACCES;
	if (!state->pend_valid) {
	    int verdict = vperm_fs_check_parent(tracee, state,
						W_OK | X_OK);

	    if (verdict < 0)
		return verdict;
	}
	break;

    case PR_link:
	if (capture_path(tracee, config, state, SYSARG_2) < 0)
	    return -EACCES;
	if (!state->pend_valid) {
	    int verdict = vperm_fs_check_parent(tracee, state,
						W_OK | X_OK);

	    if (verdict < 0)
		return verdict;
	}
	break;

    case PR_linkat:
	if (capture_path(tracee, config, state, SYSARG_4) < 0)
	    return -EACCES;
	if (!state->pend_valid) {
	    int verdict = vperm_fs_check_parent(tracee, state,
						W_OK | X_OK);

	    if (verdict < 0)
		return verdict;
	}
	break;

    case PR_rename:
    case PR_renameat:
    case PR_renameat2:{
	Reg old_reg = (sysnum == PR_rename) ? SYSARG_1 : SYSARG_2;
	Reg new_reg = (sysnum == PR_rename) ? SYSARG_2 : SYSARG_4;
	char host[PATH_MAX];
	const char *rel;
	VpermRoot *root;

	if (capture_path(tracee, config, state, old_reg) < 0)
	    return -EACCES;
	if (state->pend_protected)
	    return -EPERM;
	if (state->pend_valid) {
	    int verdict = check_parent_write(state->pend_root,
					     state->pend_rel,
					     state->uid, state->gid);

	    if (verdict < 0)
		return verdict;
	} else {
	    int verdict = vperm_fs_check_parent(tracee, state,
						W_OK | X_OK);

	    if (verdict < 0)
		return verdict;
	}

	(void) new_reg;
	(void) host;
	(void) rel;
	(void) root;

	/*
	 * The destination is the last path canonicalized for this
	 * syscall; it cannot be re-read from SYSARG_4/2 here.
	 */
	if (state->last_valid && state->last_root != NULL) {
	    state->pend_new_root = state->last_root;
	    snprintf(state->pend_new_rel, sizeof(state->pend_new_rel),
		     "%s", state->last_rel);
	    state->pend_new_valid = true;

	    /*
	     * The source of a rename is checked above, but nothing must be
	     * able to *replace* the database either -- not even through a
	     * hard link to it.
	     */
	    if (relative_of(state->last_root, state->last_host,
			    state->last_rel) == NULL)
		return -EACCES;
	}

	/* Both parents must be writable for a rename(2).  */
	if (state->last_fs_root && state->last_host[0] != '\0') {
	    int verdict = vperm_fs_parent(tracee, state, state->last_host,
					  W_OK | X_OK);

	    if (verdict < 0)
		return verdict;
	}
	break;
    }

    /*
     * Virtual identity: emulated so the host credentials never change.
     * Only the virtual root may change it; another id can only pass the
     * same value back (a no-op).  Swapping identities goes through the
     * virtual su/sudo shims, which are checked separately.
     */
    case PR_setuid:
    case PR_setuid32:
    case PR_setfsuid:
    case PR_setfsuid32:{
	uid_t wanted = (uid_t) peek_reg(tracee, ORIGINAL, SYSARG_1);

	if (state->uid != 0 && wanted != state->uid)
	    return -EPERM;

	state->uid = wanted;
	set_sysnum(tracee, PR_void);
	state->emulate = true;
	break;
    }

    case PR_setgid:
    case PR_setgid32:
    case PR_setfsgid:
    case PR_setfsgid32:{
	gid_t wanted = (gid_t) peek_reg(tracee, ORIGINAL, SYSARG_1);

	if (state->uid != 0 && wanted != state->gid)
	    return -EPERM;

	state->gid = wanted;
	set_sysnum(tracee, PR_void);
	state->emulate = true;
	break;
    }

    case PR_setreuid:
    case PR_setreuid32:
    case PR_setresuid:
    case PR_setresuid32:{
	int real = (int) peek_reg(tracee, ORIGINAL, SYSARG_1);
	int effective = (int) peek_reg(tracee, ORIGINAL, SYSARG_2);
	int saved = (sysnum == PR_setresuid || sysnum == PR_setresuid32)
	    ? (int) peek_reg(tracee, ORIGINAL, SYSARG_3) : -1;

	if (state->uid != 0
	    && ((real != -1 && (uid_t) real != state->uid)
		|| (effective != -1 && (uid_t) effective != state->uid)
		|| (saved != -1 && (uid_t) saved != state->uid)))
	    return -EPERM;

	if (real != -1)
	    state->uid = (uid_t) real;
	if (effective != -1)
	    state->uid = (uid_t) effective;
	set_sysnum(tracee, PR_void);
	state->emulate = true;
	break;
    }

    case PR_setregid:
    case PR_setregid32:
    case PR_setresgid:
    case PR_setresgid32:{
	int real = (int) peek_reg(tracee, ORIGINAL, SYSARG_1);
	int effective = (int) peek_reg(tracee, ORIGINAL, SYSARG_2);
	int saved = (sysnum == PR_setresgid || sysnum == PR_setresgid32)
	    ? (int) peek_reg(tracee, ORIGINAL, SYSARG_3) : -1;

	if (state->uid != 0
	    && ((real != -1 && (gid_t) real != state->gid)
		|| (effective != -1 && (gid_t) effective != state->gid)
		|| (saved != -1 && (gid_t) saved != state->gid)))
	    return -EPERM;

	if (real != -1)
	    state->gid = (gid_t) real;
	if (effective != -1)
	    state->gid = (gid_t) effective;
	set_sysnum(tracee, PR_void);
	state->emulate = true;
	break;
    }

    case PR_setgroups:
    case PR_setgroups32:
	/* Supplementary groups are not modelled; only root may set them.  */
	if (state->uid != 0)
	    return -EPERM;
	set_sysnum(tracee, PR_void);
	state->emulate = true;
	break;

    /*
     * Process-level permissions.  Every guest process is the same host
     * user, so the kernel would let any id signal any other one; the
     * check has to happen here.
     */
    case PR_kill:
    case PR_tkill:
    case PR_rt_sigqueueinfo:{
	pid_t target = (pid_t) peek_reg(tracee, ORIGINAL, SYSARG_1);
	int verdict = vperm_check_signal(tracee, config, state, target);

	if (verdict < 0)
	    return verdict;
	break;
    }

    case PR_tgkill:
    case PR_rt_tgsigqueueinfo:{
	pid_t target = (pid_t) peek_reg(tracee, ORIGINAL, SYSARG_2);
	int verdict = vperm_check_signal(tracee, config, state, target);

	if (verdict < 0)
	    return verdict;
	break;
    }

    case PR_pidfd_send_signal:{
	int pidfd = (int) peek_reg(tracee, ORIGINAL, SYSARG_1);
	pid_t target = vperm_pidfd_target(tracee, pidfd);

	if (target <= 0) {
	    if (state->uid != 0)
		return -EPERM;
	    break;
	}
	if (vperm_check_signal(tracee, config, state, target) < 0)
	    return -EPERM;
	break;
    }

    case PR_process_vm_readv:
    case PR_process_vm_writev:{
	pid_t target = (pid_t) peek_reg(tracee, ORIGINAL, SYSARG_1);

	if (!vperm_same_identity(config, state, target))
	    return -EPERM;
	break;
    }

    case PR_ptrace:
	/* Debugging is another way to read and write a process.  */
	if (state->uid != 0)
	    return -EPERM;
	break;

    case PR_capset:
	/* Capabilities are not virtualized: the host process never has
	 * any, and a non-root id must not be able to claim some.  */
	if (state->uid != 0)
	    return -EPERM;
	set_sysnum(tracee, PR_void);
	state->emulate = true;
	break;

    default:
	break;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Stat structure patching                                             */
/* ------------------------------------------------------------------ */

static Reg stat_address_arg(Sysnum sysnum)
{
    if (sysnum == PR_statx)
	return SYSARG_5;
    if (sysnum == PR_newfstatat || sysnum == PR_fstatat64)
	return SYSARG_3;
    if (sysnum == PR_stat || sysnum == PR_lstat
	|| sysnum == PR_stat64 || sysnum == PR_lstat64
	|| sysnum == PR_oldstat || sysnum == PR_oldlstat)
	return SYSARG_2;
    return SYSARG_2;
}

static void patch_stat(Tracee *tracee, const VpermEntry *entry, Sysnum sysnum)
{
    word_t address = peek_reg(tracee, ORIGINAL, stat_address_arg(sysnum));
    uint32_t value;

    if (sysnum == PR_statx) {
	/* stx_mode is the full mode, type bits included.  */
	uint16_t mode = (uint16_t) entry->mode;

	value = entry->uid;
	(void) write_data(tracee, address + OFFSETOF_STATX_UID, &value,
			  sizeof(value));
	value = entry->gid;
	(void) write_data(tracee, address + OFFSETOF_STATX_GID, &value,
			  sizeof(value));
	(void) write_data(tracee, address + 28, &mode, sizeof(mode));
	return;
    }

    {
	off_t mode_offset = is_32on64_mode(tracee)
	    ? 16 : offsetof(struct stat, st_mode);
	uint32_t mode = (uint32_t) entry->mode;

	(void) write_data(tracee, address + mode_offset, &mode,
			  is_32on64_mode(tracee) ? sizeof(uint32_t)
			  : sizeof(mode_t));
	value = entry->uid;
	(void) write_data(tracee, address + offsetof_stat_uid(tracee), &value,
			  sizeof(value));
	value = entry->gid;
	(void) write_data(tracee, address + offsetof_stat_gid(tracee), &value,
			  sizeof(value));
    }
}

static int handle_exit_end(Tracee *tracee, VpermConfig *config)
{
    VpermTracee *state = find_tracee_state(config, tracee->pid);
    Sysnum sysnum = get_sysnum(tracee, ORIGINAL);
    word_t result;

    if (state == NULL)
	return 0;

    result = peek_reg(tracee, CURRENT, SYSARG_RESULT);

    if (state->emulate) {
	poke_reg(tracee, SYSARG_RESULT, 0);
	state->emulate = false;
	return 0;
    }

    /*
     * A block-backed root keeps its metadata in its own filesystem, so
     * the values reported to the guest are read from there instead of
     * the virtual database.
     */
    if (state->pend_fs_backed && state->pend_host[0] != '\0') {
	switch (sysnum) {
	case PR_stat:
	case PR_stat64:
	case PR_lstat:
	case PR_lstat64:
	case PR_oldstat:
	case PR_oldlstat:
	case PR_fstat:
	case PR_fstat64:
	case PR_newfstatat:
	case PR_fstatat64:
	case PR_statx:{
	    struct stat st;

	    if ((int) result == 0
		&& netfs_block_stat(tracee, state->pend_host, &st) == 0) {
		VpermEntry entry;

		memset(&entry, 0, sizeof(entry));
		entry.mode = st.st_mode;
		entry.uid = st.st_uid;
		entry.gid = st.st_gid;
		patch_stat(tracee, &entry, sysnum);
	    }
	    return 0;
	}
	default:
	    break;
	}
    }

    /* Identity getters have no pending path state either.  */
    switch (sysnum) {
    case PR_getuid:
    case PR_getuid32:
    case PR_geteuid:
    case PR_geteuid32:
	poke_reg(tracee, SYSARG_RESULT, state->uid);
	return 0;

    case PR_getgid:
    case PR_getgid32:
    case PR_getegid:
    case PR_getegid32:
	poke_reg(tracee, SYSARG_RESULT, state->gid);
	return 0;

    case PR_getgroups:
    case PR_getgroups32:
	poke_reg(tracee, SYSARG_RESULT, 0);
	return 0;

    case PR_getresuid:
    case PR_getresuid32:
    case PR_getresgid:
    case PR_getresgid32:{
	uint32_t value = (sysnum == PR_getresuid
			  || sysnum == PR_getresuid32)
	    ? (uint32_t) state->uid : (uint32_t) state->gid;
	word_t addresses[3];
	int i;

	addresses[0] = peek_reg(tracee, ORIGINAL, SYSARG_1);
	addresses[1] = peek_reg(tracee, ORIGINAL, SYSARG_2);
	addresses[2] = peek_reg(tracee, ORIGINAL, SYSARG_3);

	for (i = 0; i < 3; i++) {
	    if (addresses[i] != 0)
		(void) write_data(tracee, addresses[i], &value,
				  sizeof(value));
	}
	poke_reg(tracee, SYSARG_RESULT, 0);
	return 0;
    }

    default:
	break;
    }

    /*
     * Directory listings carry no path argument, hence no pending state;
     * they are handled from the descriptor table instead.
     */
    if ((!state->pend_valid || state->pend_root == NULL)
	&& sysnum != PR_getdents && sysnum != PR_getdents64)
	return 0;

    switch (sysnum) {
    case PR_stat:
    case PR_stat64:
    case PR_lstat:
    case PR_lstat64:
    case PR_oldstat:
    case PR_oldlstat:
    case PR_fstat:
    case PR_fstat64:
    case PR_newfstatat:
    case PR_fstatat64:
    case PR_statx:{
	VpermEntry *entry;

	if (result != 0)
	    break;

	entry = entry_find(state->pend_root, state->pend_rel);
	if (entry != NULL)
	    patch_stat(tracee, entry, sysnum);
	break;
    }

    case PR_rename:
    case PR_renameat:
    case PR_renameat2:
	/*
	 * The destination is only known from the HOST_PATH capture, and
	 * the source may not even have an entry; relocate whatever the
	 * subtree had once the rename succeeded.
	 */
	if ((int) result == 0 && state->pend_valid && state->pend_new_valid
	    && state->pend_root == state->pend_new_root) {
	    entry_move_tree(state->pend_root, state->pend_rel,
			    state->pend_new_rel);
	    (void) db_save(state->pend_root);
	}
	break;

    case PR_unlink:
    case PR_rmdir:
    case PR_unlinkat:
	/*
	 * The entry is deliberately kept although the path is gone: it
	 * records the virtual owner of that path, and is refreshed when
	 * the path is created again.  The database is left untouched.
	 */
	break;

    case PR_mkdir:
    case PR_mkdirat:{
	word_t mode = (sysnum == PR_mkdir)
	    ? peek_reg(tracee, ORIGINAL, SYSARG_2)
	    : peek_reg(tracee, ORIGINAL, SYSARG_3);

	if ((int) result == 0) {
	    VpermEntry *entry =
		entry_find(state->pend_root, state->pend_rel);

	    if (entry != NULL) {
		/* The path already had an entry in a previous life:
		 * keep the recorded virtual owner, take the permission
		 * bits from mkdir(2), and make it a directory again.  */
		entry->mode = S_IFDIR | (mode & 07777);
	    } else {
		remember_change(state->pend_root, state->pend_rel,
				mode, true, state->uid, true,
				state->gid, true);
		entry = entry_find(state->pend_root, state->pend_rel);
		if (entry != NULL)
		    entry->mode = (entry->mode & 07777) | S_IFDIR;
	    }
	    (void) db_save(state->pend_root);
	}
	break;
    }

    case PR_symlink:
    case PR_symlinkat:
	if ((int) result == 0)
	    refresh_entry_type(state, S_IFLNK, NULL);
	break;

    case PR_link:
    case PR_linkat:
	if ((int) result == 0)
	    refresh_entry_type(state, 0, state->pend_host);
	break;

    case PR_getdents:
    case PR_getdents64:{
	int fd = (int) peek_reg(tracee, ORIGINAL, SYSARG_1);
	VpermFd *item = fd_find(state, fd);

	if (item != NULL && item->root->db_rel != NULL && (int) result > 0) {
	    const char *db = item->root->db_rel;
	    const char *slash = strrchr(db, '/');
	    const char *base = slash != NULL ? slash + 1 : db;
	    char parent[PATH_MAX];

	    if (slash == NULL)
		parent[0] = '\0';
	    else {
		size_t length = (size_t) (slash - db);

		memcpy(parent, db, length);
		parent[length] = '\0';
	    }

	    /* Only the directory that actually holds the database.  */
	    if (strcmp(item->rel, parent) == 0)
		filter_dirent(tracee, base,
			      peek_reg(tracee, ORIGINAL, SYSARG_2),
			      (int) result, sysnum == PR_getdents64,
			      is_32on64_mode(tracee));
	}
	break;
    }

    case PR_open:
    case PR_openat:
    case PR_creat:{
	word_t flags;
	Reg mode_reg;

	if (sysnum == PR_open) {
	    flags = peek_reg(tracee, ORIGINAL, SYSARG_2);
	    mode_reg = SYSARG_3;
	} else if (sysnum == PR_openat) {
	    flags = peek_reg(tracee, ORIGINAL, SYSARG_3);
	    mode_reg = SYSARG_4;
	} else {
	    flags = O_CREAT | O_WRONLY | O_TRUNC;
	    mode_reg = SYSARG_2;
	}

	if ((int) result >= 0) {
	    VpermEntry *entry =
		entry_find(state->pend_root, state->pend_rel);
	    word_t mode = peek_reg(tracee, ORIGINAL, mode_reg);

	    if ((flags & O_CREAT) != 0 && state->pend_absent) {
		if (entry != NULL) {
		    /* The path was removed but its entry was kept:
		     * keep the recorded virtual owner, take the
		     * permission bits from open(2), make it a regular
		     * file again.  */
		    entry->mode = S_IFREG | (mode & 07777);
		} else {
		    /* A newly created path becomes an entry owned by
		     * the container's virtual identity.  */
		    remember_change(state->pend_root, state->pend_rel,
				    mode, true, state->uid, true,
				    state->gid, true);
		}
		(void) db_save(state->pend_root);
	    } else if (!state->pend_absent && entry == NULL
		       && (flags & O_CREAT) != 0) {
		/* Pre-existing path without an entry yet.  */
		remember_change(state->pend_root, state->pend_rel,
				mode, true, state->uid, true,
				state->gid, true);
	    }

	    fd_add(state, (int) result, state->pend_root, state->pend_rel);
	}
	break;
    }

    case PR_dup:
    case PR_dup2:
    case PR_dup3:
	if ((int) result >= 0)
	    fd_duplicate(state, (int) peek_reg(tracee, ORIGINAL, SYSARG_1),
			 (int) result);
	break;

    case PR_fcntl:
    case PR_fcntl64:{
	int command = (int) peek_reg(tracee, ORIGINAL, SYSARG_2);

	if ((int) result >= 0
	    && (command == F_DUPFD || command == F_DUPFD_CLOEXEC))
	    fd_duplicate(state, (int) peek_reg(tracee, ORIGINAL, SYSARG_1),
			 (int) result);
	break;
    }

    default:
	break;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Extension plumbing                                                  */
/* ------------------------------------------------------------------ */

static FilteredSysnum vperm_sysnums[] = {
    { PR_chmod, FILTER_SYSEXIT },
    { PR_fchmod, FILTER_SYSEXIT },
    { PR_fchmodat, FILTER_SYSEXIT },
    { PR_fchmodat2, FILTER_SYSEXIT },
    { PR_chown, FILTER_SYSEXIT },
    { PR_chown32, FILTER_SYSEXIT },
    { PR_lchown, FILTER_SYSEXIT },
    { PR_lchown32, FILTER_SYSEXIT },
    { PR_fchown, FILTER_SYSEXIT },
    { PR_fchown32, FILTER_SYSEXIT },
    { PR_fchownat, FILTER_SYSEXIT },
    { PR_stat, FILTER_SYSEXIT },
    { PR_stat64, FILTER_SYSEXIT },
    { PR_lstat, FILTER_SYSEXIT },
    { PR_lstat64, FILTER_SYSEXIT },
    { PR_fstat, FILTER_SYSEXIT },
    { PR_fstat64, FILTER_SYSEXIT },
    { PR_fstatat64, FILTER_SYSEXIT },
    { PR_newfstatat, FILTER_SYSEXIT },
    { PR_statx, FILTER_SYSEXIT },
    { PR_open, FILTER_SYSEXIT },
    { PR_openat, FILTER_SYSEXIT },
    { PR_creat, FILTER_SYSEXIT },
    { PR_close, 0 },
    { PR_dup, FILTER_SYSEXIT },
    { PR_dup2, FILTER_SYSEXIT },
    { PR_dup3, FILTER_SYSEXIT },
    { PR_fcntl, FILTER_SYSEXIT },
    { PR_fcntl64, FILTER_SYSEXIT },
    { PR_access, FILTER_SYSEXIT },
    { PR_faccessat, FILTER_SYSEXIT },
    { PR_faccessat2, FILTER_SYSEXIT },
    { PR_execve, FILTER_SYSEXIT },
    { PR_unlink, FILTER_SYSEXIT },
    { PR_unlinkat, FILTER_SYSEXIT },
    { PR_mkdir, FILTER_SYSEXIT },
    { PR_mkdirat, FILTER_SYSEXIT },
    { PR_rmdir, FILTER_SYSEXIT },
    { PR_truncate, FILTER_SYSEXIT },
    { PR_truncate64, FILTER_SYSEXIT },
    { PR_ftruncate, FILTER_SYSEXIT },
    { PR_ftruncate64, FILTER_SYSEXIT },
    { PR_symlink, FILTER_SYSEXIT },
    { PR_symlinkat, FILTER_SYSEXIT },
    { PR_link, FILTER_SYSEXIT },
    { PR_linkat, FILTER_SYSEXIT },
    { PR_rename, FILTER_SYSEXIT },
    { PR_renameat, FILTER_SYSEXIT },
    { PR_renameat2, FILTER_SYSEXIT },
    { PR_getdents, FILTER_SYSEXIT },
    { PR_getdents64, FILTER_SYSEXIT },
    /* Virtual identity: reported to the guest and changed by su/sudo.  */
    { PR_getuid, FILTER_SYSEXIT },
    { PR_getuid32, FILTER_SYSEXIT },
    { PR_geteuid, FILTER_SYSEXIT },
    { PR_geteuid32, FILTER_SYSEXIT },
    { PR_getgid, FILTER_SYSEXIT },
    { PR_getgid32, FILTER_SYSEXIT },
    { PR_getegid, FILTER_SYSEXIT },
    { PR_getegid32, FILTER_SYSEXIT },
    { PR_getresuid, FILTER_SYSEXIT },
    { PR_getresuid32, FILTER_SYSEXIT },
    { PR_getresgid, FILTER_SYSEXIT },
    { PR_getresgid32, FILTER_SYSEXIT },
    { PR_getgroups, FILTER_SYSEXIT },
    { PR_getgroups32, FILTER_SYSEXIT },
    { PR_setuid, FILTER_SYSEXIT },
    { PR_setuid32, FILTER_SYSEXIT },
    { PR_setgid, FILTER_SYSEXIT },
    { PR_setgid32, FILTER_SYSEXIT },
    { PR_setreuid, FILTER_SYSEXIT },
    { PR_setreuid32, FILTER_SYSEXIT },
    { PR_setregid, FILTER_SYSEXIT },
    { PR_setregid32, FILTER_SYSEXIT },
    { PR_setresuid, FILTER_SYSEXIT },
    { PR_setresuid32, FILTER_SYSEXIT },
    { PR_setresgid, FILTER_SYSEXIT },
    { PR_setresgid32, FILTER_SYSEXIT },
    { PR_setfsuid, FILTER_SYSEXIT },
    { PR_setfsuid32, FILTER_SYSEXIT },
    { PR_setfsgid, FILTER_SYSEXIT },
    { PR_setfsgid32, FILTER_SYSEXIT },
    { PR_setgroups, FILTER_SYSEXIT },
    { PR_setgroups32, FILTER_SYSEXIT },
    /* Process-level permissions: enter-only checks.  */
    { PR_kill, 0 },
    { PR_tkill, 0 },
    { PR_tgkill, 0 },
    { PR_rt_sigqueueinfo, 0 },
    { PR_rt_tgsigqueueinfo, 0 },
    { PR_pidfd_send_signal, 0 },
    { PR_process_vm_readv, 0 },
    { PR_process_vm_writev, 0 },
    { PR_ptrace, 0 },
    /* capset is emulated as a no-op for root, so it needs an exit stop.  */
    { PR_capset, FILTER_SYSEXIT },
    FILTERED_SYSNUM_END,
};

static VpermConfig *config_of(Extension *extension)
{
    return talloc_get_type_abort(extension->config, VpermConfig);
}

static VpermConfig *ensure_extension(Tracee *tracee, Extension **out)
{
    Extension *extension = get_extension(tracee, vperm_callback);
    int status;

    if (extension == NULL) {
	status = initialize_extension(tracee, vperm_callback, NULL);
	if (status < 0)
	    return NULL;
	extension = get_extension(tracee, vperm_callback);
	if (extension == NULL)
	    return NULL;
    }

    if (out != NULL)
	*out = extension;
    return config_of(extension);
}

int vperm_callback(Extension *extension, ExtensionEvent event, intptr_t d1,
		   intptr_t d2 UNUSED)
{
    switch (event) {
    case INITIALIZATION:
	if (extension->config == NULL) {
	    VpermConfig *config = talloc_zero(extension, VpermConfig);

	    if (config == NULL)
		return -1;
	    /* Default virtual identity: unchanged, i.e. the real one.  */
	    config->uid = getuid();
	    config->gid = getgid();
	    extension->config = config;
	}
	extension->filtered_sysnums = vperm_sysnums;
	return 0;

    case INHERIT_PARENT:{
	Tracee *parent = TRACEE(extension);
	Tracee *child = (Tracee *) d1;
	VpermConfig *config = config_of(extension);
	VpermTracee *parent_state =
	    find_tracee_state(config, parent->pid);
	VpermTracee *child_state =
	    get_tracee_state(config, child->pid, true);

	if (parent_state != NULL && child_state != NULL) {
	    child_state->uid = parent_state->uid;
	    child_state->gid = parent_state->gid;
	}
	return 0;
    }

    case HOST_PATH:{
	Tracee *tracee = TRACEE(extension);
	VpermConfig *config = config_of(extension);
	VpermTracee *state;
	const char *rel;
	VpermRoot *root;

	if (!(bool) d2)
	    return 0;

	state = get_tracee_state(config, tracee->pid, true);
	if (state == NULL)
	    return 0;

	if (roots_build(tracee, config) < 0)
	    return -1;

	root = find_root(config, (const char *) d1, &rel);
	if (root == NULL || relative_of(root, (const char *) d1, rel) == NULL)
	    return 0;

	state->last_root = root;
	snprintf(state->last_rel, sizeof(state->last_rel), "%s", rel);
	snprintf(state->last_host, sizeof(state->last_host), "%s",
		 (const char *) d1);
	state->last_fs_root = root->fs_backed
	    && root->guest != NULL && strcmp(root->guest, "/") == 0;
	state->last_valid = true;
	return 0;
    }

    case SYSCALL_ENTER_START:
	return handle_enter_start(TRACEE(extension), config_of(extension));

    case SYSCALL_ENTER_END:
	return handle_enter_end(TRACEE(extension), config_of(extension), d1);

    case SYSCALL_EXIT_END:
	return handle_exit_end(TRACEE(extension), config_of(extension));

    case REMOVED:
	return 0;

    case PRINT_CONFIG:{
	VpermConfig *config = config_of(extension);
	VpermRoot *root;

	for (root = config->roots; root != NULL; root = root->next)
	    note(TRACEE(extension), INFO, USER, "vperm = %s (%s)",
		 root->db, root->guest);
	return 0;
    }

    default:
	return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Command line interface                                              */
/* ------------------------------------------------------------------ */

uid_t vperm_initial_uid(Tracee *tracee)
{
    Extension *extension = get_extension(tracee, vperm_callback);

    if (extension == NULL || extension->config == NULL)
	return getuid();

    return config_of(extension)->uid;
}

int vperm_enable(Tracee *tracee)
{
    VpermConfig *config = ensure_extension(tracee, NULL);

    if (config == NULL)
	return -1;

    return 0;
}

int vperm_finalize(Tracee *tracee)
{
    Extension *extension = get_extension(tracee, vperm_callback);
    VpermConfig *config;

    if (extension == NULL)
	return 0;

    config = config_of(extension);

    vmap_setup(tracee, config);

    /*
     * Load the databases now, before the guest starts: a database that
     * is unreadable or damaged must stop uvroot rather than let it run
     * with the virtual ownership silently missing.
     */
    if (roots_build(tracee, config) < 0) {
	note(tracee, ERROR, USER,
	     "vperm: refusing to start with a damaged database");
	return -1;
    }

    return 0;
}

int vperm_set_no_shims(Tracee *tracee)
{
    VpermConfig *config = ensure_extension(tracee, NULL);

    if (config == NULL)
	return -1;

    config->no_shims = true;
    return 0;
}

int vperm_set_map(Tracee *tracee, const char *value)
{
    VpermConfig *config;

    if (value == NULL || value[0] != '/') {
	note(tracee, ERROR, USER,
	     "vperm: the mapping directory must be absolute: \"%s\"",
	     value != NULL ? value : "");
	return -1;
    }

    config = ensure_extension(tracee, NULL);
    if (config == NULL)
	return -1;

    /* The shims may already have been prepared from the default
     * directory (--vperm normally comes first): rebuild them from the
     * new one.  */
    vmap_clear_users(config);
    config->shims_ready = false;

    TALLOC_FREE(config->map_dir);
    config->map_dir = talloc_strdup(config, value);
    if (config->map_dir == NULL)
	return -1;

    return 0;
}

int vperm_set_file(Tracee *tracee, const char *value)
{
    VpermConfig *config;

    if (value == NULL || value[0] != '/') {
	note(tracee, ERROR, USER,
	     "vperm: the database path must be absolute: \"%s\"",
	     value != NULL ? value : "");
	return -1;
    }

    config = ensure_extension(tracee, NULL);
    if (config == NULL)
	return -1;

    TALLOC_FREE(config->explicit_db);
    config->explicit_db = talloc_strdup(config, value);
    return config->explicit_db != NULL ? 0 : -1;
}

int vperm_set_id(Tracee *tracee, const char *value)
{
    VpermConfig *config;
    unsigned int uid;
    unsigned int gid;

    if (value == NULL) {
	note(tracee, ERROR, USER, "vperm: option expects \"uid\" or "
	     "\"uid:gid\"");
	return -1;
    }

    /* A bare user id is accepted and used for the group too.  */
    if (sscanf(value, "%u:%u", &uid, &gid) != 2) {
	if (sscanf(value, "%u", &uid) != 1) {
	    note(tracee, ERROR, USER,
		 "vperm: option expects \"uid\" or \"uid:gid\", got \"%s\"",
		 value);
	    return -1;
	}
	gid = uid;
    }

    config = ensure_extension(tracee, NULL);
    if (config == NULL)
	return -1;

    config->uid = (uid_t) uid;
    config->gid = (gid_t) gid;
    return 0;
}
