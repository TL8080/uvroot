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

- `libarchive <https://libarchive.org>`_
- `libtalloc <https://talloc.samba.org>`_
- `uthash <https://troydhanson.github.io/uthash>`_ (only required for building CARE)

Manuals
=======

- `uvroot <doc/uvroot/manual.rst>`_
- `CARE <doc/care/manual.rst>`_

Environment
===========

uvroot must be told where to put its temporary files on systems without
``/tmp`` (Android/Termux)::

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
