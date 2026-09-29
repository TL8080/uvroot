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
 * QCOW2 block backend.
 *
 * A QCOW2 image is a copy-on-write container for a raw disk image.  It
 * is translated on the fly into a byte-addressable device, so the
 * ext2/3/4 driver (fs_ext2.c) can be stacked on top of it and the
 * container sees an ordinary disk.  Everything happens in user space,
 * with no privilege, loop device, /dev/nbd or /dev/fuse access.
 *
 * The implementation covers uncompressed QCOW2 version 2 and 3 images
 * without backing file, snapshot or encryption, which is what
 * "qemu-img create -f qcow2" and "qemu-img convert -O qcow2" produce by
 * default:
 *
 *   - reads map L1 -> L2 -> data clusters; unallocated clusters read as
 *     zeroes and compressed clusters are inflated with zlib when it is
 *     available;
 *   - writes are copy-on-write: shared clusters are cloned and their
 *     refcount decremented, private ones are updated in place, and new
 *     data/L2/refcount clusters are allocated through the refcount
 *     table on demand.
 *
 * Like the other image backends, ownership and permissions live in the
 * filesystem stored inside the image, never in .uvroot-vperm.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <talloc.h>

#include "build.h"
#include "cli/note.h"
#include "extension/netfs/netfs.h"

#ifdef HAVE_ZLIB
#include <zlib.h>
#define QCOW2_HAVE_ZLIB 1
#endif

static const char *const qcow2_schemes[] = {
    "qcow2", "qcow", "qemu+img", NULL
};

#define QCOW2_MAGIC		0x514649fbU
#define QCOW2_OFFSET_MASK	0x00fffffffffffe00ULL
#define QCOW2_OFLAG_COMPRESSED	(1ULL << 62)
#define QCOW2_OFLAG_COPIED	(1ULL << 63)

#define QCOW2_MAX_L1_BYTES	(64ULL * 1024 * 1024)
#define QCOW2_MAX_CACHED_L2	64
#define QCOW2_MAX_CACHED_RB	64
#define QCOW2_HEADER_SIZE	104

#define QCOW2_INCOMPAT_DIRTY		(1ULL << 0)
#define QCOW2_INCOMPAT_CORRUPT		(1ULL << 1)
#define QCOW2_INCOMPAT_EXTERNAL_DATA	(1ULL << 2)
#define QCOW2_INCOMPAT_COMPRESSION_TYPE	(1ULL << 3)

/* One cached L2 table.  */
typedef struct Qcow2L2 {
    uint64_t offset;		/* cluster offset, 0 when not allocated */
    uint64_t *entries;		/* cluster_size / 8 host-order entries */
    bool dirty;
    uint64_t used;
} Qcow2L2;

/* One cached refcount block.  */
typedef struct Qcow2Rb {
    uint64_t offset;
    uint8_t *raw;		/* cluster_size bytes, big-endian entries */
    bool dirty;
    uint64_t used;
} Qcow2Rb;

typedef struct Qcow2Data {
    int fd;
    bool read_only;

    uint32_t version;
    uint32_t cluster_bits;
    uint64_t cluster_size;
    uint64_t virtual_size;
    uint64_t image_clusters;

    uint32_t l1_size;
    uint64_t l1_table_offset;
    uint64_t *l1;
    bool l1_dirty;

    uint64_t refcount_table_offset;
    uint32_t refcount_table_clusters;
    uint32_t refcount_bits;
    uint64_t rb_entries;	/* clusters covered by one refcount block */
    uint64_t refcount_table_size;	/* number of entries */
    uint64_t *refcount_table;
    bool refcount_table_dirty;

    uint64_t l2_entries;
    Qcow2L2 *l2;
    unsigned int l2_cached;
    uint64_t l2_clock;

    /* Layout of a compressed cluster descriptor.  */
    int csize_shift;
    uint64_t csize_mask;
    uint64_t compressed_offset_mask;

    Qcow2Rb *rb;
    uint64_t rb_count;
    unsigned int rb_cached;
    uint64_t rb_clock;

    uint64_t alloc_hint;

    /* zlib, resolved lazily and only for compressed clusters.  */
    void *zlib_handle;
    bool zlib_tried;
#ifdef QCOW2_HAVE_ZLIB
    int (*inflate_init2) (z_streamp stream, int window_bits,
			  const char *version, int stream_size);
    int (*inflate_run) (z_streamp stream, int flush);
    int (*inflate_end) (z_streamp stream);
#endif
} Qcow2Data;

/* ------------------------------------------------------------------ */
/* Byte order and low level I/O                                        */
/* ------------------------------------------------------------------ */

static uint16_t be16_get(const void *pointer)
{
    const uint8_t *p = pointer;

    return (uint16_t) (((uint16_t) p[0] << 8) | (uint16_t) p[1]);
}

static uint32_t be32_get(const void *pointer)
{
    const uint8_t *p = pointer;

    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
	| ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static uint64_t be64_get(const void *pointer)
{
    const uint8_t *p = pointer;

    return ((uint64_t) be32_get(p) << 32) | (uint64_t) be32_get(p + 4);
}

static void be16_put(void *pointer, uint16_t value)
{
    uint8_t *p = pointer;

    p[0] = (uint8_t) (value >> 8);
    p[1] = (uint8_t) value;
}

static void be32_put(void *pointer, uint32_t value)
{
    uint8_t *p = pointer;

    p[0] = (uint8_t) (value >> 24);
    p[1] = (uint8_t) (value >> 16);
    p[2] = (uint8_t) (value >> 8);
    p[3] = (uint8_t) value;
}

static void be64_put(void *pointer, uint64_t value)
{
    be32_put(pointer, (uint32_t) (value >> 32));
    be32_put((uint8_t *) pointer + 4, (uint32_t) value);
}

/* Convert an array of big-endian 64-bit entries to host order, in place.  */
static void be64_array_import(uint64_t *entries, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++)
	entries[i] = be64_get(&entries[i]);
}

/* Return a host-order array converted to big-endian for the disk.  */
static uint64_t *be64_array_export(const uint64_t *entries, size_t count,
				   TALLOC_CTX *context)
{
    uint64_t *raw = talloc_array(context, uint64_t, (unsigned int) count);
    size_t i;

    if (raw == NULL)
	return NULL;

    for (i = 0; i < count; i++)
	be64_put(&raw[i], entries[i]);

    return raw;
}

static int read_at(Qcow2Data *data, void *buffer, size_t length,
		   uint64_t offset)
{
    uint8_t *cursor = buffer;

    while (length > 0) {
	ssize_t got = pread(data->fd, cursor, length, (off_t) offset);

	if (got < 0) {
	    if (errno == EINTR)
		continue;
	    return -errno;
	}
	if (got == 0)
	    return -EIO;

	cursor += got;
	offset += (uint64_t) got;
	length -= (size_t) got;
    }

    return 0;
}

static int write_at(Qcow2Data *data, const void *buffer, size_t length,
		    uint64_t offset)
{
    const uint8_t *cursor = buffer;

    while (length > 0) {
	ssize_t put = pwrite(data->fd, cursor, length, (off_t) offset);

	if (put < 0) {
	    if (errno == EINTR)
		continue;
	    return -errno;
	}
	if (put == 0)
	    return -EIO;

	cursor += put;
	offset += (uint64_t) put;
	length -= (size_t) put;
    }

    return 0;
}

static int zero_at(Qcow2Data *data, uint64_t offset, uint64_t length)
{
    static const uint8_t zeroes[65536];

    while (length > 0) {
	size_t chunk = length > sizeof(zeroes) ? sizeof(zeroes)
					       : (size_t) length;

	if (write_at(data, zeroes, chunk, offset) < 0)
	    return -EIO;

	offset += chunk;
	length -= chunk;
    }

    return 0;
}

static uint64_t cluster_of(Qcow2Data *data, uint64_t entry)
{
    return (entry & QCOW2_OFFSET_MASK) / data->cluster_size;
}

static uint64_t compressed_cluster_of(Qcow2Data *data, uint64_t entry)
{
    return (entry & data->compressed_offset_mask) / data->cluster_size;
}

/* ------------------------------------------------------------------ */
/* zlib, used only to inflate compressed clusters                      */
/* ------------------------------------------------------------------ */

#ifdef QCOW2_HAVE_ZLIB

static bool load_zlib(Qcow2Data *data)
{
    const char *environment;

    if (data->zlib_tried)
	return data->inflate_run != NULL;
    data->zlib_tried = true;

    environment = getenv("UVROOT_NETFS_ZLIB");
    if (environment != NULL && environment[0] != '\0')
	data->zlib_handle = dlopen(environment, RTLD_NOW | RTLD_LOCAL);
    if (data->zlib_handle == NULL)
	data->zlib_handle = dlopen("libz.so.1", RTLD_NOW | RTLD_LOCAL);
    if (data->zlib_handle == NULL)
	data->zlib_handle = dlopen("libz.so", RTLD_NOW | RTLD_LOCAL);
    if (data->zlib_handle == NULL)
	return false;

    *(void **) (&data->inflate_init2) =
	dlsym(data->zlib_handle, "inflateInit2_");
    *(void **) (&data->inflate_run) =
	dlsym(data->zlib_handle, "inflate");
    *(void **) (&data->inflate_end) =
	dlsym(data->zlib_handle, "inflateEnd");

    return data->inflate_init2 != NULL && data->inflate_run != NULL
	&& data->inflate_end != NULL;
}

/*
 * QEMU compresses clusters with raw deflate (a negative window size),
 * but older versions and other tools use the zlib wrapper, so both are
 * tried.  Returns 0 when @out received @out_len bytes.
 */
static int inflate_stream(Qcow2Data *data, const uint8_t *source,
			  size_t source_length, uint8_t *out, size_t out_len,
			  int window_bits)
{
    z_stream stream;
    int status;

    memset(&stream, 0, sizeof(stream));
    stream.next_in = (Bytef *) source;
    stream.avail_in = (uInt) source_length;
    stream.next_out = out;
    stream.avail_out = (uInt) out_len;

    if (data->inflate_init2(&stream, window_bits, ZLIB_VERSION,
			    sizeof(stream)) != Z_OK)
	return -EIO;

    status = data->inflate_run(&stream, Z_FINISH);
    data->inflate_end(&stream);

    if (status != Z_STREAM_END || stream.total_out != out_len)
	return -EIO;

    return 0;
}

#else				/* !QCOW2_HAVE_ZLIB */

static bool load_zlib(Qcow2Data *data UNUSED)
{
    return false;
}

static int inflate_stream(Qcow2Data *data UNUSED, const uint8_t *source UNUSED,
			  size_t source_length UNUSED, uint8_t *out UNUSED,
			  size_t out_len UNUSED, int window_bits UNUSED)
{
    return -ENOTSUP;
}

#endif				/* QCOW2_HAVE_ZLIB */

/*
 * Inflate the compressed cluster described by @entry into @out, which
 * must have room for one full cluster.  Returns a negative errno.
 *
 * A compressed cluster descriptor stores the byte offset of the start
 * of the deflate stream in the low bits and the number of 512-byte
 * sectors it occupies in the high bits, with the split depending on the
 * cluster size (see qcow2_parse_compressed_l2_entry in QEMU).
 */
static int inflate_cluster(Qcow2Data *data, uint64_t entry, uint8_t *out)
{
    uint64_t source = entry & data->compressed_offset_mask;
    uint64_t sectors = ((entry >> data->csize_shift) & data->csize_mask) + 1;
    uint64_t compressed_size =
	sectors * 512 - (source & 511);
    uint8_t *compressed;
    int status;

    if (!load_zlib(data)) {
	note(NULL, ERROR, USER,
	     "netfs: this QCOW2 image contains compressed clusters but zlib "
	     "is not available; set UVROOT_NETFS_ZLIB to a libz path");
	return -ENOTSUP;
    }

    compressed = talloc_size(data, (size_t) compressed_size);
    if (compressed == NULL)
	return -ENOMEM;

    /*
     * The last cluster of an image whose size is not a cluster multiple
     * can inflate to fewer bytes than a full cluster; the remainder is
     * implicitly zero.
     */
    memset(out, 0, (size_t) data->cluster_size);

    status = read_at(data, compressed, (size_t) compressed_size, source);
    if (status == 0) {
	/* Raw deflate first (what QEMU writes), then the zlib wrapper.  */
	status = inflate_stream(data, compressed, (size_t) compressed_size,
				out, (size_t) data->cluster_size, -15);
	if (status < 0)
	    status = inflate_stream(data, compressed,
				    (size_t) compressed_size, out,
				    (size_t) data->cluster_size, 15);
    }

    TALLOC_FREE(compressed);
    return status;
}

/* ------------------------------------------------------------------ */
/* L1 table                                                            */
/* ------------------------------------------------------------------ */

static int l1_flush(Qcow2Data *data)
{
    uint64_t *raw;
    int status;

    if (!data->l1_dirty)
	return 0;

    raw = be64_array_export(data->l1, data->l1_size, data);
    if (raw == NULL)
	return -ENOMEM;

    status = write_at(data, raw, (size_t) data->l1_size * 8,
		      data->l1_table_offset);
    TALLOC_FREE(raw);
    if (status < 0)
	return status;

    data->l1_dirty = false;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Refcount blocks                                                     */
/* ------------------------------------------------------------------ */

static uint64_t rb_entry_get(const Qcow2Data *data, const Qcow2Rb *block,
			     uint64_t entry)
{
    const uint8_t *p = block->raw;

    switch (data->refcount_bits) {
    case 16:
	return be16_get(p + entry * 2);
    case 32:
	return be32_get(p + entry * 4);
    case 64:
	return be64_get(p + entry * 8);
    default:
	return 0;
    }
}

static void rb_entry_put(const Qcow2Data *data, Qcow2Rb *block, uint64_t entry,
			 uint64_t value)
{
    uint8_t *p = block->raw;

    switch (data->refcount_bits) {
    case 16:
	be16_put(p + entry * 2, (uint16_t) value);
	break;
    case 32:
	be32_put(p + entry * 4, (uint32_t) value);
	break;
    case 64:
	be64_put(p + entry * 8, value);
	break;
    default:
	break;
    }
}

static int rb_flush(Qcow2Data *data, uint64_t index)
{
    int status;

    if (index >= data->rb_count)
	return -EINVAL;
    if (!data->rb[index].dirty)
	return 0;

    status = write_at(data, data->rb[index].raw,
		      (size_t) data->cluster_size, data->rb[index].offset);
    if (status < 0)
	return status;

    data->rb[index].dirty = false;
    return 0;
}

static void rb_evict_one(Qcow2Data *data)
{
    uint64_t best = 0;
    uint64_t oldest = UINT64_MAX;
    uint64_t i;

    for (i = 0; i < data->rb_count; i++) {
	if (data->rb[i].raw == NULL)
	    continue;
	if (data->rb[i].used < oldest) {
	    oldest = data->rb[i].used;
	    best = i;
	}
    }

    if (data->rb[best].raw == NULL)
	return;

    if (data->rb[best].dirty)
	write_at(data, data->rb[best].raw, (size_t) data->cluster_size,
		 data->rb[best].offset);

    TALLOC_FREE(data->rb[best].raw);
    data->rb[best].dirty = false;
    data->rb_cached--;
}

static int rb_table(Qcow2Data *data, uint64_t index, Qcow2Rb **result)
{
    Qcow2Rb *entry;

    if (index >= data->rb_count)
	return -EINVAL;

    entry = &data->rb[index];

    if (entry->raw != NULL) {
	entry->used = ++data->rb_clock;
	*result = entry;
	return 0;
    }

    if (data->rb_cached >= QCOW2_MAX_CACHED_RB)
	rb_evict_one(data);

    entry->raw = talloc_zero_size(data, (size_t) data->cluster_size);
    if (entry->raw == NULL)
	return -ENOMEM;
    entry->used = ++data->rb_clock;
    data->rb_cached++;

    if (data->refcount_table[index] == 0) {
	entry->offset = 0;
	*result = entry;
	return 0;
    }

    entry->offset = data->refcount_table[index];
    {
	int status = read_at(data, entry->raw, (size_t) data->cluster_size,
			     entry->offset);

	if (status < 0) {
	    TALLOC_FREE(entry->raw);
	    data->rb_cached--;
	    return status;
	}
    }

    *result = entry;
    return 0;
}

static int refcount_table_flush(Qcow2Data *data)
{
    uint64_t *raw;
    int status;

    if (!data->refcount_table_dirty)
	return 0;

    raw = be64_array_export(data->refcount_table, data->refcount_table_size,
			    data);
    if (raw == NULL)
	return -ENOMEM;

    status = write_at(data, raw, (size_t) data->refcount_table_size * 8,
		      data->refcount_table_offset);
    TALLOC_FREE(raw);
    if (status < 0)
	return status;

    data->refcount_table_dirty = false;
    return 0;
}

/*
 * Allocate a cluster inside a region whose refcount block already
 * exists, so recording the new refcount cannot itself need a new
 * refcount block.  Used for metadata (L2 tables and refcount blocks).
 */
static int refcount_get(Qcow2Data *data, uint64_t cluster, uint64_t *value)
{
    uint64_t index = cluster / data->rb_entries;
    uint64_t entry = cluster % data->rb_entries;
    Qcow2Rb *block;
    int status;

    if (index >= data->refcount_table_size)
	return -ERANGE;

    if (data->refcount_table[index] == 0) {
	*value = 0;
	return 0;
    }

    status = rb_table(data, index, &block);
    if (status < 0)
	return status;

    *value = rb_entry_get(data, block, entry);
    return 0;
}

static int refcount_set(Qcow2Data *data, uint64_t cluster, uint64_t value)
{
    uint64_t index = cluster / data->rb_entries;
    uint64_t entry = cluster % data->rb_entries;
    Qcow2Rb *block;
    int status;

    if (index >= data->refcount_table_size)
	return -ERANGE;

    if (data->refcount_table[index] == 0) {
	uint64_t block_cluster;

	if (value == 0)
	    return 0;

	/*
	 * No refcount block covers this region yet, which means no
	 * cluster of it has ever been given a refcount, so all of them
	 * are free.  Take the first one for the block itself; its own
	 * refcount lives in the block being created, so nothing has to
	 * be allocated recursively.  Skip @cluster so that the caller
	 * never gets back the very cluster it is about to use as data.
	 */
	block_cluster = index * data->rb_entries;
	if (block_cluster == cluster)
	    block_cluster++;
	if (block_cluster >= data->image_clusters)
	    return -ENOSPC;

	status = rb_table(data, index, &block);
	if (status < 0)
	    return status;

	status = zero_at(data, block_cluster * data->cluster_size,
			 data->cluster_size);
	if (status < 0)
	    return status;

	data->refcount_table[index] = block_cluster * data->cluster_size;
	data->refcount_table_dirty = true;
	block->offset = data->refcount_table[index];
	rb_entry_put(data, block, block_cluster % data->rb_entries, 1);
	block->dirty = true;
    }

    status = rb_table(data, index, &block);
    if (status < 0)
	return status;

    rb_entry_put(data, block, entry, value);
    block->dirty = true;
    return 0;
}

static int refcount_dec(Qcow2Data *data, uint64_t cluster)
{
    uint64_t value;
    int status = refcount_get(data, cluster, &value);

    if (status < 0)
	return status;
    if (value == 0)
	return 0;

    return refcount_set(data, cluster, value - 1);
}

/*
 * Allocate a free data cluster and give it refcount 1.  A region whose
 * refcount block is missing still counts as free, so the whole image is
 * usable; refcount_set() creates the block when the first cluster of
 * that region is taken.
 */
static int alloc_cluster(Qcow2Data *data, uint64_t *result)
{
    uint64_t i;
    int pass;

    for (pass = 0; pass < 2; pass++) {
	uint64_t start = pass == 0 ? data->alloc_hint : 0;
	uint64_t stop = pass == 0 ? data->image_clusters : data->alloc_hint;

	for (i = start; i < stop; i++) {
	    uint64_t value;
	    int status;

	    status = refcount_get(data, i, &value);
	    if (status < 0)
		return status;
	    if (value != 0)
		continue;

	    status = refcount_set(data, i, 1);
	    if (status < 0)
		return status;

	    data->alloc_hint = i + 1;
	    *result = i;
	    return 0;
	}
    }

    return -ENOSPC;
}

/* ------------------------------------------------------------------ */
/* L2 table cache                                                      */
/* ------------------------------------------------------------------ */

static void l2_evict_one(Qcow2Data *data)
{
    uint32_t best = 0;
    uint64_t oldest = UINT64_MAX;
    uint32_t i;

    for (i = 0; i < data->l1_size; i++) {
	if (data->l2[i].entries == NULL)
	    continue;
	if (data->l2[i].used < oldest) {
	    oldest = data->l2[i].used;
	    best = i;
	}
    }

    if (data->l2[best].entries == NULL)
	return;

    if (data->l2[best].dirty && data->l2[best].offset != 0) {
	uint64_t *raw = be64_array_export(data->l2[best].entries,
					  (size_t) (data->cluster_size / 8),
					  data);

	if (raw != NULL) {
	    write_at(data, raw, (size_t) data->cluster_size,
		     data->l2[best].offset);
	    TALLOC_FREE(raw);
	}
    }

    TALLOC_FREE(data->l2[best].entries);
    data->l2[best].dirty = false;
    data->l2_cached--;
}

static int l2_flush(Qcow2Data *data, uint32_t index)
{
    uint64_t *raw;
    int status;

    if (index >= data->l1_size)
	return -EINVAL;
    if (!data->l2[index].dirty)
	return 0;
    if (data->l2[index].offset == 0)
	return -EIO;

    raw = be64_array_export(data->l2[index].entries,
			    (size_t) (data->cluster_size / 8), data);
    if (raw == NULL)
	return -ENOMEM;

    status = write_at(data, raw, (size_t) data->cluster_size,
		      data->l2[index].offset);
    TALLOC_FREE(raw);
    if (status < 0)
	return status;

    data->l2[index].dirty = false;
    return 0;
}

static int l2_table(Qcow2Data *data, uint32_t index, bool allocate,
		    Qcow2L2 **result)
{
    Qcow2L2 *entry;

    if (index >= data->l1_size)
	return -EINVAL;

    entry = &data->l2[index];

    if (entry->entries != NULL) {
	entry->used = ++data->l2_clock;
	*result = entry;
	return 0;
    }

    if (data->l2_cached >= QCOW2_MAX_CACHED_L2)
	l2_evict_one(data);

    entry->entries = talloc_zero_array(data, uint64_t,
				       (unsigned int) (data->cluster_size / 8));
    if (entry->entries == NULL)
	return -ENOMEM;
    entry->used = ++data->l2_clock;
    data->l2_cached++;

    if (data->l1[index] != 0) {
	int status;

	entry->offset = data->l1[index] & QCOW2_OFFSET_MASK;
	status = read_at(data, entry->entries, (size_t) data->cluster_size,
			 entry->offset);
	if (status < 0) {
	    TALLOC_FREE(entry->entries);
	    data->l2_cached--;
	    return status;
	}
	be64_array_import(entry->entries,
			  (size_t) (data->cluster_size / 8));
	entry->dirty = false;
	*result = entry;
	return 0;
    }

    if (!allocate) {
	entry->offset = 0;
	entry->dirty = false;
	*result = entry;
	return 0;
    }

    if (data->read_only) {
	TALLOC_FREE(entry->entries);
	data->l2_cached--;
	return -EROFS;
    }

    {
	uint64_t cluster;
	int status = alloc_cluster(data, &cluster);

	if (status < 0) {
	    TALLOC_FREE(entry->entries);
	    data->l2_cached--;
	    return status;
	}

	status = zero_at(data, cluster * data->cluster_size,
			 data->cluster_size);
	if (status < 0) {
	    TALLOC_FREE(entry->entries);
	    data->l2_cached--;
	    return status;
	}

	entry->offset = cluster * data->cluster_size;
	entry->dirty = true;
	data->l1[index] = entry->offset | QCOW2_OFLAG_COPIED;
	data->l1_dirty = true;
    }

    *result = entry;
    return 0;
}

static int l2_get_entry(Qcow2Data *data, uint64_t cluster, bool allocate,
			uint64_t *value)
{
    uint32_t index = (uint32_t) (cluster / data->l2_entries);
    uint64_t slot = cluster % data->l2_entries;
    Qcow2L2 *table;
    int status;

    status = l2_table(data, index, allocate, &table);
    if (status < 0)
	return status;

    *value = table->entries[slot];
    return 0;
}

static int l2_set_entry(Qcow2Data *data, uint64_t cluster, uint64_t value)
{
    uint32_t index = (uint32_t) (cluster / data->l2_entries);
    uint64_t slot = cluster % data->l2_entries;
    Qcow2L2 *table;
    int status;

    status = l2_table(data, index, true, &table);
    if (status < 0)
	return status;

    table->entries[slot] = value;
    table->dirty = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Block operations                                                    */
/* ------------------------------------------------------------------ */

static int qcow2_flush(NetfsMount *mount);

static ssize_t qcow2_block_pread(NetfsMount *mount, void *buffer, size_t count,
				 uint64_t offset)
{
    Qcow2Data *data = mount->backend_data;
    uint8_t *out = buffer;
    size_t wanted;

    if (data == NULL)
	return -ENODEV;
    if (count == 0)
	return 0;
    if (offset >= data->virtual_size)
	return -EINVAL;
    if (count > data->virtual_size - offset)
	count = (size_t) (data->virtual_size - offset);
    wanted = count;

    while (count > 0) {
	uint64_t cluster = offset / data->cluster_size;
	uint64_t within = offset % data->cluster_size;
	size_t chunk = (size_t) (data->cluster_size - within);
	uint64_t entry;
	int status;

	if (chunk > count)
	    chunk = count;

	status = l2_get_entry(data, cluster, false, &entry);
	if (status < 0)
	    return status;

	if (entry == 0) {
	    memset(out, 0, chunk);
	} else if ((entry & QCOW2_OFLAG_COMPRESSED) != 0) {
	    uint8_t *inflated = talloc_size(data, (size_t) data->cluster_size);

	    if (inflated == NULL)
		return -ENOMEM;

	    status = inflate_cluster(data, entry, inflated);
	    if (status < 0) {
		TALLOC_FREE(inflated);
		return status;
	    }
	    memcpy(out, inflated + within, chunk);
	    TALLOC_FREE(inflated);
	} else {
	    status = read_at(data, out, chunk,
			     (entry & QCOW2_OFFSET_MASK) + within);
	    if (status < 0)
		return status;
	}

	out += chunk;
	offset += chunk;
	count -= chunk;
    }

    return (ssize_t) wanted;
}

static ssize_t qcow2_block_pwrite(NetfsMount *mount, const void *buffer,
				  size_t count, uint64_t offset)
{
    Qcow2Data *data = mount->backend_data;
    const uint8_t *in = buffer;
    size_t wanted;

    if (data == NULL)
	return -ENODEV;
    if (data->read_only)
	return -EROFS;
    if (count == 0)
	return 0;
    if (offset >= data->virtual_size)
	return -EINVAL;
    if (count > data->virtual_size - offset)
	count = (size_t) (data->virtual_size - offset);
    wanted = count;

    while (count > 0) {
	uint64_t cluster = offset / data->cluster_size;
	uint64_t within = offset % data->cluster_size;
	size_t chunk = (size_t) (data->cluster_size - within);
	uint64_t entry;
	uint64_t target = 0;
	int status;

	if (chunk > count)
	    chunk = count;

	status = l2_get_entry(data, cluster, true, &entry);
	if (status < 0)
	    return status;

	if (entry != 0 && (entry & QCOW2_OFLAG_COMPRESSED) == 0) {
	    uint64_t owner = cluster_of(data, entry);
	    uint64_t references;

	    status = refcount_get(data, owner, &references);
	    if (status < 0)
		return status;

	    if (references == 1) {
		target = entry & QCOW2_OFFSET_MASK;
	    } else {
		uint64_t fresh;
		uint8_t *copy = talloc_size(data,
					    (size_t) data->cluster_size);

		if (copy == NULL)
		    return -ENOMEM;

		status = alloc_cluster(data, &fresh);
		if (status == 0)
		    target = fresh * data->cluster_size;
		if (status == 0)
		    status = read_at(data, copy, (size_t) data->cluster_size,
				     entry & QCOW2_OFFSET_MASK);
		if (status == 0)
		    status = write_at(data, copy, (size_t) data->cluster_size,
				      target);
		TALLOC_FREE(copy);
		if (status < 0)
		    return status;

		status = refcount_dec(data, owner);
		if (status < 0)
		    return status;
		status = l2_set_entry(data, cluster,
				      target | QCOW2_OFLAG_COPIED);
		if (status < 0)
		    return status;
	    }
	} else if (entry != 0) {
	    /* Compressed cluster: inflate it into a fresh cluster.  */
	    uint64_t owner = compressed_cluster_of(data, entry);
	    uint64_t fresh;
	    uint8_t *inflated = talloc_size(data, (size_t) data->cluster_size);

	    if (inflated == NULL)
		return -ENOMEM;

	    status = alloc_cluster(data, &fresh);
	    if (status == 0)
		target = fresh * data->cluster_size;
	    if (status == 0)
		status = inflate_cluster(data, entry, inflated);
	    if (status == 0)
		status = write_at(data, inflated, (size_t) data->cluster_size,
				  target);
	    TALLOC_FREE(inflated);
	    if (status < 0)
		return status;

	    status = refcount_dec(data, owner);
	    if (status < 0)
		return status;
	    status = l2_set_entry(data, cluster, target | QCOW2_OFLAG_COPIED);
	    if (status < 0)
		return status;
	} else {
	    uint64_t fresh;

	    status = alloc_cluster(data, &fresh);
	    if (status < 0)
		return status;

	    target = fresh * data->cluster_size;

	    /* A partial write must not expose the previous content of a
	     * recycled cluster, so clear it first.  */
	    if (within != 0 || chunk != data->cluster_size) {
		status = zero_at(data, target, data->cluster_size);
		if (status < 0)
		    return status;
	    }

	    status = l2_set_entry(data, cluster, target | QCOW2_OFLAG_COPIED);
	    if (status < 0)
		return status;
	}

	status = write_at(data, in, chunk, target + within);
	if (status < 0)
	    return status;

	in += chunk;
	offset += chunk;
	count -= chunk;
    }

    return (ssize_t) wanted;
}

static int qcow2_block_size(NetfsMount *mount, uint64_t *bytes)
{
    Qcow2Data *data = mount->backend_data;

    if (data == NULL)
	return -ENODEV;

    *bytes = data->virtual_size;
    return 0;
}

static int qcow2_flush(NetfsMount *mount)
{
    Qcow2Data *data = mount->backend_data;
    uint32_t i;

    if (data == NULL)
	return -ENODEV;

    for (i = 0; i < data->l1_size; i++) {
	if (data->l2[i].entries != NULL && data->l2[i].dirty
	    && l2_flush(data, i) < 0)
	    return -EIO;
    }

    for (i = 0; i < data->rb_count; i++) {
	if (data->rb[i].raw != NULL && data->rb[i].dirty
	    && rb_flush(data, i) < 0)
	    return -EIO;
    }

    if (refcount_table_flush(data) < 0 || l1_flush(data) < 0)
	return -EIO;

    if (!data->read_only && data->fd >= 0 && fsync(data->fd) < 0)
	return -errno;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Opening and closing                                                 */
/* ------------------------------------------------------------------ */

static const char *qcow2_path(const char *url)
{
    if (strncasecmp(url, "qcow2://", 8) == 0)
	return url + 8;
    if (strncasecmp(url, "qcow://", 7) == 0)
	return url + 7;
    if (strncasecmp(url, "qemu+img://", 11) == 0)
	return url + 11;
    return url;
}

static void qcow2_fini(NetfsMount *mount)
{
    Qcow2Data *data = mount->backend_data;

    if (data != NULL) {
	qcow2_flush(mount);
	if (data->fd >= 0)
	    close(data->fd);
	if (data->zlib_handle != NULL)
	    dlclose(data->zlib_handle);
	TALLOC_FREE(data);
    }
    mount->backend_data = NULL;
}

static int qcow2_init(NetfsMount *mount, const char *url)
{
    const char *path = qcow2_path(url);
    uint8_t header[QCOW2_HEADER_SIZE];
    uint64_t incompatible;
    uint32_t order;
    Qcow2Data *data;
    int status;

    if (path == NULL || path[0] == '\0') {
	note(NULL, ERROR, USER, "netfs: no image path given for %s", url);
	return -EINVAL;
    }

    data = talloc_zero(mount, Qcow2Data);
    if (data == NULL)
	return -ENOMEM;
    data->fd = -1;

    data->fd = open(path, O_RDWR | O_CLOEXEC);
    if (data->fd < 0) {
	data->fd = open(path, O_RDONLY | O_CLOEXEC);
	data->read_only = true;
    }
    if (data->fd < 0) {
	status = -errno;
	note(NULL, ERROR, USER, "netfs: cannot open image \"%s\": %s",
	     path, strerror(-status));
	TALLOC_FREE(data);
	return status;
    }

    status = netfs_lock_image(data->fd, !data->read_only, path);
    if (status < 0) {
	close(data->fd);
	TALLOC_FREE(data);
	return status;
    }

    status = read_at(data, header, sizeof(header), 0);
    if (status < 0) {
	note(NULL, ERROR, USER, "netfs: cannot read the header of \"%s\"",
	     path);
	goto fail;
    }

    if (be32_get(header) != QCOW2_MAGIC) {
	note(NULL, ERROR, USER, "netfs: \"%s\" is not a QCOW2 image", path);
	status = -ENODEV;
	goto fail;
    }

    data->version = be32_get(header + 4);
    if (data->version != 2 && data->version != 3) {
	note(NULL, ERROR, USER,
	     "netfs: unsupported QCOW2 version %u in \"%s\"",
	     data->version, path);
	status = -ENOTSUP;
	goto fail;
    }

    if (be64_get(header + 8) != 0) {
	note(NULL, ERROR, USER,
	     "netfs: \"%s\" has a backing file, which is not supported", path);
	status = -ENOTSUP;
	goto fail;
    }

    data->cluster_bits = be32_get(header + 20);
    if (data->cluster_bits < 9 || data->cluster_bits > 21) {
	note(NULL, ERROR, USER, "netfs: corrupt QCOW2 cluster size in \"%s\"",
	     path);
	status = -EIO;
	goto fail;
    }
    data->cluster_size = 1ULL << data->cluster_bits;

    data->virtual_size = be64_get(header + 24);
    if (data->virtual_size == 0) {
	note(NULL, ERROR, USER, "netfs: empty QCOW2 image \"%s\"", path);
	status = -EIO;
	goto fail;
    }

    if (be32_get(header + 32) != 0) {
	note(NULL, ERROR, USER,
	     "netfs: encrypted QCOW2 images are not supported (\"%s\")", path);
	status = -ENOTSUP;
	goto fail;
    }

    data->l1_size = be32_get(header + 36);
    data->l1_table_offset = be64_get(header + 40);
    data->refcount_table_offset = be64_get(header + 48);
    data->refcount_table_clusters = be32_get(header + 56);

    if (be32_get(header + 60) != 0) {
	note(NULL, ERROR, USER,
	     "netfs: QCOW2 snapshots are not supported (\"%s\")", path);
	status = -ENOTSUP;
	goto fail;
    }

    if (data->version >= 3) {
	incompatible = be64_get(header + 72);
	order = be32_get(header + 96);
	if (order > 6) {
	    note(NULL, ERROR, USER,
		 "netfs: unsupported QCOW2 refcount order %u in \"%s\"",
		 order, path);
	    status = -ENOTSUP;
	    goto fail;
	}
	data->refcount_bits = 1U << order;
    } else {
	incompatible = 0;
	data->refcount_bits = 16;
    }

    if ((incompatible & QCOW2_INCOMPAT_CORRUPT) != 0) {
	note(NULL, ERROR, USER,
	     "netfs: \"%s\" is marked corrupt; run \"qemu-img check\"", path);
	status = -EIO;
	goto fail;
    }
    if ((incompatible & QCOW2_INCOMPAT_EXTERNAL_DATA) != 0) {
	note(NULL, ERROR, USER,
	     "netfs: \"%s\" uses an external data file, which is not supported",
	     path);
	status = -ENOTSUP;
	goto fail;
    }
    if ((incompatible & QCOW2_INCOMPAT_COMPRESSION_TYPE) != 0) {
	note(NULL, ERROR, USER,
	     "netfs: \"%s\" uses an unsupported compression type", path);
	status = -ENOTSUP;
	goto fail;
    }
    if ((incompatible & QCOW2_INCOMPAT_DIRTY) != 0) {
	note(NULL, WARNING, USER,
	     "netfs: \"%s\" was not closed cleanly, clearing the dirty flag",
	     path);
	if (!data->read_only) {
	    uint8_t cleared[8];

	    be64_put(cleared, incompatible & ~QCOW2_INCOMPAT_DIRTY);
	    if (write_at(data, cleared, sizeof(cleared), 72) < 0) {
		status = -EIO;
		goto fail;
	    }
	}
    }

    if (data->refcount_bits != 16 && data->refcount_bits != 32
	&& data->refcount_bits != 64) {
	note(NULL, ERROR, USER,
	     "netfs: unsupported QCOW2 refcount width %u in \"%s\"",
	     data->refcount_bits, path);
	status = -ENOTSUP;
	goto fail;
    }

    if (data->l1_table_offset == 0 || data->refcount_table_offset == 0
	|| data->l1_size == 0 || data->refcount_table_clusters == 0) {
	note(NULL, ERROR, USER, "netfs: corrupt QCOW2 metadata in \"%s\"",
	     path);
	status = -EIO;
	goto fail;
    }

    if ((uint64_t) data->l1_size * 8 > QCOW2_MAX_L1_BYTES) {
	note(NULL, ERROR, USER, "netfs: \"%s\" has an absurdly large L1 table",
	     path);
	status = -E2BIG;
	goto fail;
    }

    data->l2_entries = data->cluster_size / 8;
    data->rb_entries = data->cluster_size * 8 / data->refcount_bits;
    data->csize_shift = 62 - ((int) data->cluster_bits - 8);
    data->csize_mask = (1ULL << (data->cluster_bits - 8)) - 1;
    data->compressed_offset_mask = (1ULL << data->csize_shift) - 1;
    data->refcount_table_size =
	(uint64_t) data->refcount_table_clusters * (data->cluster_size / 8);
    data->image_clusters =
	(data->virtual_size + data->cluster_size - 1) / data->cluster_size;

    if ((uint64_t) data->l1_size * data->l2_entries < data->image_clusters) {
	note(NULL, ERROR, USER,
	     "netfs: the L1 table of \"%s\" is too small for its size", path);
	status = -EIO;
	goto fail;
    }
    if (data->refcount_table_size * data->rb_entries
	< data->image_clusters) {
	note(NULL, ERROR, USER,
	     "netfs: the refcount table of \"%s\" does not cover its size",
	     path);
	status = -EIO;
	goto fail;
    }

    data->l1 = talloc_zero_array(data, uint64_t,
				 (unsigned int) data->l1_size);
    data->refcount_table = talloc_zero_array(data, uint64_t,
					     (unsigned int)
					     data->refcount_table_size);
    data->l2 = talloc_zero_array(data, Qcow2L2,
				 (unsigned int) data->l1_size);
    data->rb = talloc_zero_array(data, Qcow2Rb,
				 (unsigned int) data->refcount_table_size);
    if (data->l1 == NULL || data->refcount_table == NULL
	|| data->l2 == NULL || data->rb == NULL) {
	status = -ENOMEM;
	goto fail;
    }
    data->rb_count = data->refcount_table_size;

    status = read_at(data, data->l1, (size_t) data->l1_size * 8,
		     data->l1_table_offset);
    if (status < 0) {
	note(NULL, ERROR, USER, "netfs: cannot read the L1 table of \"%s\"",
	     path);
	goto fail;
    }
    be64_array_import(data->l1, data->l1_size);

    status = read_at(data, data->refcount_table,
		     (size_t) data->refcount_table_size * 8,
		     data->refcount_table_offset);
    if (status < 0) {
	note(NULL, ERROR, USER,
	     "netfs: cannot read the refcount table of \"%s\"", path);
	goto fail;
    }
    be64_array_import(data->refcount_table, data->refcount_table_size);

    mount->backend_data = data;
    mount->custom_io = true;

    VERBOSE(NULL, 1, "netfs: QCOW2 v%u image \"%s\", %llu bytes, "
	    "%llu-byte clusters", data->version, path,
	    (unsigned long long) data->virtual_size,
	    (unsigned long long) data->cluster_size);

    return 0;

fail:
    if (data->fd >= 0)
	close(data->fd);
    TALLOC_FREE(data);
    return status;
}

const NetfsBackend netfs_backend_qcow2 = {
    .name = "qcow2",
    .schemes = qcow2_schemes,
    .kind = NETFS_KIND_BLOCK,
    .implemented = true,
    .block = {
	    .open = NULL,	/* opened by qcow2_init() */
	    .close = qcow2_fini,
	    .size = qcow2_block_size,
	    .pread = qcow2_block_pread,
	    .pwrite = qcow2_block_pwrite,
	    .flush = qcow2_flush,
	    },
    .init = qcow2_init,
    .fini = qcow2_fini,
};
