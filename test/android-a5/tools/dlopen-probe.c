/*
 * dlopen-probe -- report which of the libraries uvroot's netfs backends
 * dlopen() can actually be loaded by a bionic (Android) process.
 *
 * The candidate order, the PREFIX hint and the required symbols mirror
 * the netfs backends in src/extension/netfs/, so "OK" here means the
 * corresponding uvroot backend would find and use the library (uvroot
 * itself still has to be built with the matching headers).
 *
 * Build for the device with the NDK, e.g.
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/\
 *       aarch64-linux-android24-clang -O2 -o dlopen-probe dlopen-probe.c -ldl
 *
 * Run it as the Termux user so that PREFIX is set, with the directories
 * holding the libraries on LD_LIBRARY_PATH (the sonames are looked up by
 * name):
 *   LD_LIBRARY_PATH=$PREFIX/lib:$PREFIX/glibc/lib:/data/local/tmp/lib \
 *       ./dlopen-probe
 */
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct backend {
	const char *feature;	/* what the library enables */
	const char *library;	/* the library, for the report */
	const char *env;	/* override variable, or NULL */
	const char *unversioned;	/* name below $PREFIX/lib, or NULL */
	const char *const *sonames;	/* names tried after that, NULL-ended */
	const char *const *symbols;	/* what the backend dlsym()s */
};

/* --- libcurl: --ftp/--ftps/--sftp ------------------------------------- */
static const char *const curl_sonames[] = { "libcurl.so.4", "libcurl.so", NULL };
static const char *const curl_symbols[] = {
	"curl_global_init", "curl_global_cleanup", "curl_easy_init",
	"curl_easy_cleanup", "curl_easy_setopt", "curl_easy_perform",
	"curl_easy_strerror", "curl_slist_append", "curl_slist_free_all", NULL
};

/* --- libsmbclient: --smb ---------------------------------------------- */
static const char *const smb_sonames[] = { "libsmbclient.so.0", "libsmbclient.so", NULL };
static const char *const smb_symbols[] = {
	"smbc_new_context", "smbc_init_context", "smbc_free_context",
	"smbc_set_context", "smbc_setFunctionAuthData", "smbc_setDebug",
	"smbc_opendir", "smbc_readdir", "smbc_closedir", "smbc_stat",
	"smbc_open", "smbc_read", "smbc_write", "smbc_close",
	"smbc_unlink", "smbc_mkdir", "smbc_rmdir", "smbc_rename", NULL
};

/* --- libnfs: --nfs ---------------------------------------------------- */
static const char *const nfs_sonames[] = { "libnfs.so.16", "libnfs.so", NULL };
static const char *const nfs_symbols[] = {
	"nfs_init_context", "nfs_destroy_context", "nfs_get_error",
	"nfs_set_timeout", "nfs_set_autoreconnect", "nfs_set_uid",
	"nfs_set_gid", "nfs_set_version", "nfs_set_nfsport",
	"nfs_set_mountport", "nfs_parse_url_dir", "nfs_destroy_url",
	"nfs_mount", "nfs_umount", "nfs_stat64", "nfs_open", "nfs_open2",
	"nfs_close", "nfs_pread", "nfs_pwrite", "nfs_fsync", "nfs_ftruncate",
	"nfs_opendir", "nfs_readdir", "nfs_closedir", "nfs_mkdir2",
	"nfs_rmdir", "nfs_unlink", "nfs_rename", NULL
};

/* --- libext2fs: --img/--raw/--file and every block root --------------- */
static const char *const ext2_sonames[] = { "libext2fs.so.2", "libext2fs.so", NULL };
static const char *const ext2_symbols[] = {
	"ext2fs_open", "ext2fs_close", "ext2fs_read_inode",
	"ext2fs_dir_iterate", "ext2fs_lookup", "ext2fs_file_open",
	"ext2fs_file_read", "ext2fs_file_close", "error_message",
	"ext2fs_read_bitmaps", "ext2fs_flush", "ext2fs_write_inode",
	"ext2fs_write_new_inode", "ext2fs_new_inode", "ext2fs_link",
	"ext2fs_unlink", "ext2fs_mkdir", "ext2fs_symlink",
	"ext2fs_is_fast_symlink", "ext2fs_inode_alloc_stats2", "ext2fs_punch",
	"ext2fs_write_inode_bitmap", "ext2fs_file_write",
	"ext2fs_file_set_size2", "unix_io_manager", NULL
};

/* --- libnbd: --nbd ---------------------------------------------------- */
static const char *const nbd_sonames[] = { "libnbd.so.0", "libnbd.so", NULL };
static const char *const nbd_symbols[] = {
	"nbd_create", "nbd_close", "nbd_get_error", "nbd_get_errno",
	"nbd_set_export_name", "nbd_set_uri_allow_tls",
	"nbd_set_uri_allow_local_file", "nbd_set_request_block_size",
	"nbd_set_pread_initialize", "nbd_connect_uri", "nbd_connect_unix",
	"nbd_get_size", "nbd_can_flush", "nbd_pread", "nbd_pwrite",
	"nbd_flush", "nbd_shutdown", NULL
};

/* --- libiscsi: --iscsi ------------------------------------------------ */
static const char *const iscsi_sonames[] = {
	"libiscsi.so.11", "libiscsi.so.0", "libiscsi.so", NULL
};
static const char *const iscsi_symbols[] = {
	"iscsi_create_context", "iscsi_destroy_context", "iscsi_set_targetname",
	"iscsi_set_session_type", "iscsi_set_header_digest", "iscsi_set_timeout",
	"iscsi_set_initiator_username_pwd", "iscsi_full_connect_sync",
	"iscsi_disconnect", "iscsi_get_error", "iscsi_read16_sync",
	"iscsi_write16_sync", "iscsi_readcapacity16_sync",
	"scsi_free_scsi_task", NULL
};

/* --- zlib: compressed QCOW2 clusters ---------------------------------- */
static const char *const zlib_sonames[] = { "libz.so.1", "libz.so", NULL };
static const char *const zlib_symbols[] = {
	"inflateInit2_", "inflate", "inflateEnd", NULL
};

static const struct backend backends[] = {
	{ "--ftp/--ftps/--sftp", "libcurl", "UVROOT_NETFS_LIBCURL",
	  "libcurl.so", curl_sonames, curl_symbols },
	{ "--smb", "libsmbclient", "UVROOT_NETFS_LIBSMBCLIENT",
	  "libsmbclient.so", smb_sonames, smb_symbols },
	{ "--nfs", "libnfs", "UVROOT_NETFS_LIBNFS",
	  "libnfs.so", nfs_sonames, nfs_symbols },
	{ "--img/--raw/--file", "libext2fs", NULL,
	  NULL, ext2_sonames, ext2_symbols },
	{ "--nbd", "libnbd", "UVROOT_NETFS_LIBNBD",
	  "libnbd.so", nbd_sonames, nbd_symbols },
	{ "--iscsi", "libiscsi", "UVROOT_NETFS_LIBISCSI",
	  "libiscsi.so", iscsi_sonames, iscsi_symbols },
	{ "--qcow2 (compressed)", "zlib", "UVROOT_NETFS_ZLIB",
	  NULL, zlib_sonames, zlib_symbols },
};

/*
 * Once dlopen() succeeded we still want to know *which* file the linker
 * picked -- /system/lib64/libext2fs.so and a Termux one would look the
 * same otherwise.  Look the name up in /proc/self/maps.
 */
static void resolve_loaded_path(const char *name, char *out, size_t outsz)
{
	const char *base = strrchr(name, '/');
	FILE *maps;
	char line[4096];

	base = base != NULL ? base + 1 : name;
	out[0] = '\0';

	maps = fopen("/proc/self/maps", "r");
	if (maps == NULL)
		return;

	while (fgets(line, sizeof(line), maps) != NULL) {
		char *hit = strstr(line, base);
		char *start;

		if (hit == NULL)
			continue;
		start = hit;
		while (start > line && start[-1] != ' ' && start[-1] != '\n')
			start--;
		if (*start != '/')
			continue;
		snprintf(out, outsz, "%s", start);
		out[strcspn(out, "\n")] = '\0';
		break;
	}
	fclose(maps);
}

static void probe(const struct backend *b)
{
	char prefix_path[PATH_MAX];
	char resolved[PATH_MAX];
	const char *prefix = getenv("PREFIX");
	const char *override = b->env != NULL ? getenv(b->env) : NULL;
	const char *loaded = NULL;
	void *handle = NULL;
	int found = 0, missing = 0;
	size_t i;

	if (override != NULL && override[0] != '\0') {
		loaded = override;
		handle = dlopen(override, RTLD_NOW | RTLD_LOCAL);
	}
	if (handle == NULL && b->unversioned != NULL
	    && prefix != NULL && prefix[0] != '\0') {
		snprintf(prefix_path, sizeof(prefix_path), "%s/lib/%s",
			 prefix, b->unversioned);
		loaded = prefix_path;
		handle = dlopen(prefix_path, RTLD_NOW | RTLD_LOCAL);
	}
	for (i = 0; handle == NULL && b->sonames[i] != NULL; i++) {
		loaded = b->sonames[i];
		handle = dlopen(b->sonames[i], RTLD_NOW | RTLD_LOCAL);
	}

	if (handle == NULL) {
		const char *err = dlerror();

		printf("FAIL  %-22s %-14s  %s\n", b->feature, b->library,
		       err != NULL ? err : "not found");
		return;
	}

	for (i = 0; b->symbols[i] != NULL; i++) {
		if (dlsym(handle, b->symbols[i]) != NULL)
			found++;
		else {
			missing++;
			printf("      missing symbol: %s\n", b->symbols[i]);
		}
	}

	resolve_loaded_path(loaded, resolved, sizeof(resolved));
	printf("%s  %-22s %-14s  %s (%d/%d symbols)\n",
	       missing == 0 ? "OK  " : "PART", b->feature, b->library,
	       resolved[0] != '\0' ? resolved : loaded, found, found + missing);
	dlclose(handle);
}

int main(void)
{
	const char *prefix = getenv("PREFIX");
	size_t i;

	printf("PREFIX=%s\n", prefix != NULL ? prefix : "(unset)");
	printf("LD_LIBRARY_PATH=%s\n",
	       getenv("LD_LIBRARY_PATH") != NULL ? getenv("LD_LIBRARY_PATH") : "(unset)");
	for (i = 0; i < sizeof(backends) / sizeof(backends[0]); i++)
		probe(&backends[i]);
	return 0;
}
