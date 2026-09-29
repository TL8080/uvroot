uvroot
======

*用户态虚拟根 — chroot, mount --bind, and binfmt_misc without privilege/setup*

uvroot is a user-mode virtual root for Linux and Android/Termux: it gives an
unprivileged process its own root file system, bind mounts, fake identities and
a set of virtual file-system backends (network shares, disk images, iSCSI/NBD)
without requiring root, kernel modules or any system-wide setup.

It is a fork of `PRoot <https://github.com/proot-me/proot>`_ and keeps the
original ptrace-based design; everything below the "What this fork adds"
section is inherited from upstream.

Heritage and attribution
========================

This project is derived from PRoot, originally written at STMicroelectronics
and maintained by the PRoot developers.  upstream project, documentation and
issue tracker:

- Source: https://github.com/proot-me/proot
- Website: https://proot-me.github.io
- Mailing list: proot_me@googlegroups.com
- Chat: https://gitter.im/proot-me/devs

The original copyright notices, the AUTHORS file and the GPL-2.0-or-later
licence are kept unchanged.  Bug reports about the *upstream* behaviour should
go to the upstream tracker linked above.

What this fork adds
===================

- ``--multi``: several containers in one process, sharing the same image
- virtual permission database (``--vperm``/``--vperm-id``) recording virtual
  uid/gid/mode without touching host metadata
- read-only enforcement (``--read-only`` / ``--ro=<path>``)
- user-mode network/disk backends: FTP/FTPS/SFTP, SMB, NFS, iSCSI, NBD,
  ext2/3/4 images and qcow2 (``--netfs=`` and friends)
- ``/proc`` and ``df`` storage-information virtualisation for virtual roots

A detailed, flag-by-flag comparison with upstream PRoot — plus usage recipes —
is in `CHANGES-vs-PRoot.md <CHANGES-vs-PRoot.md>`_.

Quick start
===========

::

    # Alpine (musl) rootfs
    curl -LO https://mirrors.ustc.edu.cn/alpine/latest-stable/releases/aarch64/alpine-minirootfs-3.24.2-aarch64.tar.gz
    mkdir -p ~/alpine && tar -xzf alpine-minirootfs-*-aarch64.tar.gz -C ~/alpine

    # enter it as a fake root (no privileges needed)
    uvroot -r ~/alpine -i 0:0 -w / -b /dev -b /proc -b /sys /bin/sh

``-R <rootfs>`` is shorthand for ``-r`` plus the recommended bindings, and
``-S <rootfs>`` adds the fake root identity.  Note that ``-0``/``--root-id``
does not exist here: use ``-i 0:0``.

Bind mounts, network/image backends, virtual permissions and read-only roots::

    uvroot -r ~/ubuntu -i 0:0 -b ~/work:/work -w /work /bin/bash
    uvroot -r / --smb=/mnt/s:'smb://user:pass@host/share' -i 0:0 /bin/sh
    uvroot -r ~/alpine --vperm --vperm-id=0:0 /bin/sh
    uvroot -r ~/ubuntu -i 0:0 --read-only --ro=/tmp /bin/bash

A disk image or an iSCSI/NBD device used as the guest root
(``--img=/:...``, ``--iscsi=/:...``) contains none of the host's
directories, so pass ``-w /`` explicitly: otherwise uvroot warns that the
inherited working directory does not exist in the guest and falls back to
``/``.

On Android/Termux there is no ``/tmp``, so set ``UVROOT_TMP_DIR``; see
`CHANGES-vs-PRoot.md <CHANGES-vs-PRoot.md>`_ section 6.4.

Compiling
=========

The following commands can be used to compile uvroot and CARE::

    make -C src loader.elf loader-m32.elf build.h # first build the config and loader
    make -C src uvroot care # then compile uvroot and CARE
    make -C test # run test suite

Cross-compiling for Android/arm64 with the Android NDK is covered in
``test/android-a5/README.md`` (a ready-made script lives next to it).

Dependencies
============

Linked into the build:

- `libtalloc <https://talloc.samba.org>`_ — required by uvroot
- `libarchive <https://libarchive.org>`_ — only for CARE
- `uthash <https://troydhanson.github.io/uthash>`_ — only for CARE

User-mode I/O drivers
---------------------

The ``netfs`` drivers are not linked against their libraries: each is
resolved with ``dlopen()`` when its option is used, so uvroot keeps working
where they are missing (that driver then reports it is not available, the
rest is unaffected).  For libcurl, libsmbclient, libnfs, libnbd and
libiscsi, ``$PREFIX/lib/<name>.so`` is tried first when ``PREFIX`` is set,
as Termux does.

- ``--ftp``, ``--ftps``, ``--sftp`` (``ftp://``, ``ftps://``, ``ftpes://``,
  ``sftp://``) — **libcurl**, tried as ``libcurl.so.4``, ``libcurl.so``;
  override with ``UVROOT_NETFS_LIBCURL``.
- ``--smb`` (``smb://``, ``cifs://``) — **libsmbclient**, tried as
  ``libsmbclient.so.0``, ``libsmbclient.so``; ``UVROOT_NETFS_LIBSMBCLIENT``.
- ``--nfs`` (``nfs://``) — **libnfs**, tried as ``libnfs.so.16``,
  ``libnfs.so``; ``UVROOT_NETFS_LIBNFS``.
- ``--img``, ``--raw``, ``--file`` (``img://``, ``raw://``, ``file://``) —
  **libext2fs** for the ext2/3/4 driver, tried as ``libext2fs.so.2``,
  ``libext2fs.so`` (no override variable).
- ``--nbd`` (``nbd://``, ``nbd+unix://``, ``nbds://``) — **libnbd**, tried
  as ``libnbd.so.0``, ``libnbd.so``; ``UVROOT_NETFS_LIBNBD``.
- ``--iscsi`` (``iscsi://``) — **libiscsi**, tried as ``libiscsi.so.11``,
  ``libiscsi.so.0``, ``libiscsi.so``; ``UVROOT_NETFS_LIBISCSI``.
- ``--qcow2`` compressed clusters — **zlib**, tried as ``libz.so.1``,
  ``libz.so``; ``UVROOT_NETFS_ZLIB``.

At build time the Makefile probes ``pkg-config`` for ``libcurl``,
``smbclient``, ``ext2fs``, ``libnbd``, ``libiscsi``, ``libnfs`` and
``zlib``; a driver is only compiled when those headers are present,
otherwise it becomes a stub that reports the feature as unavailable.

On Termux the libraries come from ``libcurl``, ``samba`` (which provides
``libsmbclient``), ``libnfs``, ``e2fsprogs`` (which provides
``libext2fs``) and ``zlib``; ``libnbd`` and ``libiscsi`` are not packaged
there.

Manuals
=======

- `uvroot <doc/uvroot/manual.rst>`_
- `CARE <doc/care/manual.rst>`_

Environment
===========

Without ``/tmp`` (Android/Termux) the temporary directory is selected from
``UVROOT_TMP_DIR``, then ``TMPDIR``, then a path baked in at build time —
``test/android-a5/build-android-ndk.sh`` compiles in ``$PREFIX/tmp`` — and
finally ``/tmp``.  Export it explicitly when none of those fits::

    export UVROOT_TMP_DIR=/data/data/com.termux/files/usr/tmp

Other knobs, all prefixed with ``UVROOT_``:

- ``UVROOT_TMP_DIR`` — temporary directory for the embedded loader
- ``UVROOT_NO_SECCOMP`` — disable the seccomp acceleration
- ``UVROOT_IGNORE_MISSING_BINDINGS`` — do not fail on missing ``-b`` sources
- ``UVROOT_FORCE_KOMPAT``, ``UVROOT_FORCE_FOREIGN_BINARY``
- ``UVROOT_NETFS_*`` — backend selection and tuning

Support
=======

This is a private fork: there is no dedicated mailing list or chat.  For the
upstream project use the links in the "Heritage and attribution" section.

License
=======

SPDX-License-Identifier: `GPL-2.0-or-later <COPYING>`_

Same licence as upstream PRoot.  The original copyright notices and
`AUTHORS <AUTHORS>`_ are kept unchanged.

AI attribution
==============

Part of this fork's revision (the rename, documentation and the ``faccessat2``
fix) was produced by an AI coding agent.  Per the common practice of disclosing
AI involvement, it is identified as:

- **AI identifier: ``deepseek-flash``** (DeepSeek Harness coding agent)

See `AI-ATTRIBUTION.md <AI-ATTRIBUTION.md>`_ for the exact scope and for the
``Co-Authored-By`` trailer used in the commits.
