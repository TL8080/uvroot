Changelog
=========

All notable changes to this project will be documented in this file.

The format is based on `Keep a Changelog`_, and this project adheres to
`Semantic Versioning`_.

Unreleased
----------

Changes that have not been released are listed below.

Added
~~~~~

- The ``netfs`` extension: user-space virtual mounts for FTP/FTPS/SFTP
  (``--ftp``), SMB/CIFS (``--smb``) and NFS (``--nfs``), plus a generic
  ``--netfs`` option.  Remote directories are mirrored on demand into a
  private local cache that is bound into the guest, so no privilege,
  kernel module or ``/dev/fuse`` access is needed.  File contents are
  fetched when opened for reading and pushed back when the last
  descriptor is closed; ``mkdir``, ``rmdir``, ``unlink`` and ``rename``
  are forwarded to the remote side.
- ``netfs`` block backends: raw images (``--img``/``--raw``/``--file``),
  QCOW2 images (``--qcow2``), NBD exports (``--nbd``) and iSCSI LUNs
  (``--iscsi``).  They are interpreted by a built-in user-space
  ext2/3/4 driver, so the ownership and permission bits stored inside
  the disk are reported to and enforced on the guest, with no
  ``.uvroot-vperm`` database.  Any of them can serve as the guest root.
  libnfs, libnbd, libiscsi, libext2fs and zlib are resolved with
  ``dlopen()``, and the QCOW2 translation (including COW writes,
  refcount maintenance and compressed clusters) is implemented in tree.
- ``--overlay-root=<bind|block>,<lower>`` overlays the guest root itself
  over a read-only lower tree.  The guest root selected by ``-r`` is the
  upper, so a root overlay and one ordinary ``--overlay`` can be used
  together.  The three-field ``mode,upper,lower`` form is also accepted.
  A root overlay may put its upper inside the lower tree, so
  ``-r tmp --overlay-root=block,/`` is valid.
- ``--overlay=<bind|block>,<guest>,<type1>,<type2>`` presents a writable
  type 1 over a read-only type 2.  Reads take type 1 first and fall back
  to type 2; new files and modified files stay in type 1.  Bind mode
  copies a whole file up, while block mode stores only the modified
  4 KiB blocks in a sparse type 1 file.  An ``.overlay`` database in the
  type 1 root records whiteouts and, for block mode, the logical size,
  mode, modified ranges and SHA-256 of the merged content.  ``/dev/bkc``,
  ``/dev/bme`` and ``/dev/brd`` inspect, merge and deduplicate deltas;
  ``brd -r`` recursively drops blocks/files identical to type 2; merging is
  allowed for the virtual root or the file owner when ``--vperm`` is
  enabled.  Either layer may be a host directory or an
  ``img://``/``qcow2://``/``nbd://``/``iscsi://``/network source URI:
  the source is materialized into a staging tree, type 2 is a read-only
  snapshot, and type 1 changes are pushed back to the source.  A
  source-backed type 1 does not support ``/dev/bme`` yet.
- Two containers can now use the same image at the same time without
  corrupting it.  The first process to open the image owns its
  filesystem driver and serves the others, which forward their directory
  operations over a Unix socket instead of opening the file a second
  time.  This keeps a single copy of the block and inode bitmaps, so
  concurrent writers (and several virtual identities) are safe.  The
  permission rules still apply across the service.
- ``vperm`` identity switching is now bounded: only the virtual root may
  change the virtual identity; another id can only pass its own value
  back.  Without the virtual ``su``/``sudo`` shims an unprivileged id
  cannot switch at all, and nobody can use them to become root unless
  already root.  ``capset``, ``ptrace`` and ``process_vm_*`` are
  intercepted too, so a container process cannot climb out through
  another privilege path.
- Processes are now permission-managed per virtual identity: an id
  other than the virtual root may only signal processes of its own
  identity (``kill``, ``tkill``, ``tgkill``, ``rt_sigqueueinfo``,
  ``rt_tgsigqueueinfo``, ``pidfd_send_signal``, including process
  groups).  The host kernel cannot decide this because every guest
  process runs as the same host user.
- On a block root the ``vperm`` layer now enforces the permissions stored
  inside the image for real: traversal of every ancestor directory,
  ``unlink``/``rmdir``/``mkdir``/``rename``/``symlink``/``link`` through
  the parent directory, ``truncate``, and ``chmod``/``chown`` ownership.
  Previously only ``open``/``creat`` were checked, so a file created by
  the virtual root could be read, rewritten or deleted by any other id.
  A block *data* mount (``--img=/mnt/disk:disk.img`` under a real rootfs)
  is left alone: its guest runs as the real host user.
- A trailing slash in a path (``mkdir -p a/b`` passes ``a/``) no longer
  makes the remote helper treat the basename as empty, which used to
  lose newly created files and directories silently.
- ``netfs`` now forwards ``symlink`` and ``link`` for block backends,
  which store them inside the filesystem carried by the disk; directory
  transports keep rejecting them with ``EPERM``.  Kernel-side copies
  (``copy_file_range``, ``sendfile``, ``splice``, ``tee``) mark the
  destination dirty so that ``cat`` and friends write back correctly.
- Pluggable backend registry for ``netfs`` covering the directory and
  block transports above, plus a filesystem-driver layer for block
  backends (ext2/3/4 implemented; FAT/exFAT and NTFS reserved).
- The ``vperm`` extension (``--vperm``, ``--vperm-file``,
  ``--vperm-id``): a persistent per-root database of virtual uid/gid/mode
  that the ``stat()`` family reports and that ``open()``/``access()``/
  ``execve()`` enforce, while ``chmod()``/``chown()`` only update the
  database and never touch the host.  Entries without a record fall
  through to the host metadata; stale entries are dropped and backed up
  at startup, and creation/unlink/rmdir/rename keep the database in sync.
- A per-process virtual identity for ``vperm``: the
  ``setuid``/``setgid``/``setresuid``/``setresgid`` family changes only
  the virtual identity (host credentials are never touched) and the
  ``getuid``/``getgid`` family reports it, inherited across fork/exec.
  Virtual ``su`` and ``sudo`` shims are generated into a switchable
  mapping directory (``--vperm-map``) and bound into the container, so
  users can be switched there.
- ``vperm`` exposes a writable virtual ``/etc/passwd`` backed by the
  mapping directory, blocks the container from deleting, overwriting or
  renaming the su/sudo shims, and ``--vperm-nosu`` disables the virtual
  su/sudo entirely (the identity is then fixed at container start).
- ``netfs`` now refuses ``ftp://``/``smb://`` as the guest root: those
  protocols cannot represent symlinks, ownership and permission bits
  faithfully.  Data mounts keep working and
  ``UVROOT_NETFS_ALLOW_REMOTE_ROOT=1`` overrides the check.
- Ancestor execute and parent-directory write checks for ``vperm``,
  whole-subtree entry relocation on directory renames, and the metadata
  database is now filtered out of directory listings.
- Cross-compiled builds can merge the headers and pkg-config files of
  dlopen-only libraries (such as libiscsi, behind ``--iscsi``) into their
  sysroot through ``EXTRA_DEPS_DIRS`` -- without them the matching backend
  is compiled as a stub.

Changed
~~~~~~~

- ``-b /:...`` is rejected: the host root may only be supplied by
  ``-r``, ``--overlay-root`` or a virtual/network disk root.  A
  subdirectory may still be written as ``-b /ce:/``, but overriding the
  root already selected by ``-r`` is unsupported and known to be buggy;
  ``-b`` should only be used for subpaths.
- ``vperm`` now treats ``-b`` bind mounts as separate vperm roots, each
  with its own database under the per-user data directory.  A bind-mounted
  file now reports and accepts virtual ownership/mode instead of being
  passed straight to the host.  The special ``-b /:/`` root binding still
  follows the binding conflict rule (the last binding for ``/`` wins).
- ``vperm`` now enforces execute permission from the virtual mode before
  ``execve(2)``.  A virtual ``700`` file runs even when the host inode is
  stored as ``600``, while a virtual ``600`` file is refused even when the
  host inode is ``700``.  The owner execute bit is added to the host inode
  only when execution actually needs it; ``chmod``/``chown`` still update
  only the virtual database.  Direct command lookup now consults vperm
  before rejecting a non-executable host file.
- ``--overlay`` mode now selects only the copy-up policy.  Existing
  entries record whether they hold a full file or a sparse block delta,
  so an upper created in ``block`` mode is readable when mounted with
  ``bind`` (and vice versa); the selected mode is saved for subsequent
  writes.  Previously a mode mismatch failed with ``EINVAL`` at mount
  time.
- ``vperm`` no longer takes over the interim ``.nvroot.vperm`` database
  name; only upstream PRoot's ``.proot-vperm`` is migrated to
  ``.uvroot-vperm``.
- The temporary directory is now selected from ``UVROOT_TMP_DIR``, then
  ``TMPDIR``, then a path baked in at build time
  (``UVROOT_TMP_DIR_DEFAULT``), and finally ``/tmp``, so a cross-built
  binary works on a target without ``/tmp`` and still reports
  ``uvroot --version``.

Fixed
~~~~~

- ``--wg=...:conf=/path`` now gives the file body to uvroot's built-in
  WireGuard engine.  Previously only an external implementation received
  the parsed file; the built-in engine saw the literal ``conf=`` string,
  reported ``no PrivateKey`` and silently fell back to NAT without
  opening the configured UDP listener.
- ``netfs`` block images no longer corrupt an ext2/3/4 filesystem when
  ``rename(2)`` replaces an existing destination.  The driver appended a
  second directory entry with the same name, so the old content kept
  shadowing the new one and ``e2fsck`` reported a non-unique file name;
  it now removes the destination first, with the type rules of
  ``rename(2)``.  This is what turned ``apk add`` and ``pip install``
  inside a qcow2-backed root into silently truncated packages.
- Removing a fast symlink on a ``netfs`` block image no longer runs the
  regular-file truncation on it, which read the inline target as block
  numbers and freed unrelated blocks (``ext2fs_block_alloc_stats:
  Illegal block number`` followed by an inconsistent bitmap).
- ``link(2)`` on a ``netfs`` block image now reports ``EEXIST`` when the
  destination already exists instead of appending a duplicate entry.
- Directory removal performed by ``rm -rf`` through
  ``unlinkat(..., AT_REMOVEDIR)`` is now mirrored to ``netfs`` and overlay
  backends just like ``rmdir(2)``; previously the directory stayed visible
  in the remote or lower tree after the next mount.
- Removing one name of a hard-linked file on a ``netfs`` block image only
  drops that link now.  The inode and its blocks were released even when
  other names still referenced them, which left an inode marked free but
  referenced and put live blocks on the free list (``e2fsck`` reported
  both bitmap differences).
- ``vperm`` keeps owner access on the host inode of a newly created file
  or directory while still storing the virtual mode.  ``dpkg`` creates
  directories with mode 0 and applies the real mode through
  ``chmod(2)``, which ``vperm`` only records; because the host inode
  stayed mode 0, the next ``stat(2)`` failed with ``EACCES`` and
  ``apt-get install`` aborted with "unable to stat ... Permission
  denied".
- ``vperm`` no longer records a directory, symlink or hard link as a
  regular file: the object type comes from ``lstat(2)``, a new symlink
  is recorded as ``S_IFLNK | 0777``, a hard link inherits the source's
  virtual owner and mode, and ``chmod``/``chown`` on a fresh path keep
  the host object's type.
- ``chown(2)``/``chgrp(2)`` with an unspecified id (``-1``) on a
  ``vperm`` path now preserves the recorded owner or group instead of
  storing uid/gid 4294967295.
- Renaming over an existing destination on a ``vperm`` path drops the
  destination's recorded metadata before moving the source entries, so
  the moved object no longer inherits stale ownership and duplicate
  database entries are not created.

5.4.1 - 2026-09-07
------------------

Added
~~~~~

- clone3 syscall support
- CodeQL static analysis workflow and Dependabot version tracking

Changed
~~~~~~~

- Reformatted all sources with indent -kr
- Modernized Docker test images: replaced EOL centos/debian bases, moved
  built images to ghcr.io, dropped to a non-root build user
- Migrated SonarCloud scanning to its automatic PR analysis and pinned
  GitHub Actions to commit SHA hashes

Fixed
~~~~~

- readlinkat(2) with an empty pathname on a dirfd opened with
  O_PATH|O_NOFOLLOW no longer aborts the tracer (#182)
- Broken Ubuntu rootfs link in the docs
- Assorted CodeQL/SonarCloud findings: unchecked write_data() return in
  the portmap extension, deprecated bzero, an invalid %z format
  specifier, and stale comments
- Static release build failing to link uvroot/care due to the Python
  extension and libarchive's transitive static dependencies

5.4.0 - 2023-05-13
------------------

Added
~~~~~

- faccessat2 syscall
- Enable SonarCloud for GitHub Actions
- Include uthash v2.3.0 as submodule
- Disable mixed execution with new --mixed-mode option

Changed
~~~~~~~

- Rename test-0cf405b0.c to fix_memory_corruption_execve_proc_self_exe.c

Fixed
~~~~~

- Android compatibility with cwd
- Running test-0cf405b0 for newer versions of glibc
- Running test-25069c12 and test-25069c13 on newer kernels

5.3.1 - 2022-04-24
------------------

Changed
~~~~~~~

- Error out when trying to set PTRACE_O_TRACESECCOMP under ptrace emulation.
- Set the restart_how field in a newly created child tracee.

Removed
~~~~~~~

- Unnecessary dependency of uvroot on libarchive.
- Changelog target from doc makefile.

Fixed
~~~~~

- Incorrect year for 5.3.0 release in changelog and manual.

5.3.0 - 2022-01-04
------------------

Added
~~~~~

- Link to repository on website.

- Support for utimensat_time64 on 32bit architectures.

- Install LZOP on CI for CARE archive extraction.

- Enable GitHub Actions for testing.

- Message for stopping and starting of tracees.

- Python 3 support in tests.

- Support for statx syscall.

- Test case for sysexit handler.

Changed
~~~~~~~

- Update wording in manual regarding rootfs.

- Change restart_original_syscall to not use chained syscall.

- Access sockfd in the chained getsocketname via the original version.

- Pin Debian 8 for docker image.

- Make sure not to fake too old an kernel release.

- Ensure the stack is aligned for AArch64 and X86 for SIMD code.

- Include /bin in PATH during tests.

- Kernel version detection for kernels 5.0 and newer.

- Allow a higher initial heap size in test.

- Allow the value of AT_HWCAP to be empty.

- Do not unconditionally use PTRACE_CONT when recieving a useless SECCOMP event.

- Do not treat libarchive warnings as errors.

- canon: call bindings substitution on '/' component of user path.

Removed
~~~~~~~

- Remove special handling of syscall avoider number on ARM.

- Delete roadmap.rst file.

- Remove Travis CI configuration.

- Remove preprocessor directives and associated code.

Fixed
~~~~~

- Fchmod permissions for loader.

- Test compilation on ARM.

- Includes in tests.

- Handling of receiving seccomp after normal ptrace event.

- Waitpid on zombies.

- Extraction of wrapped file.

- Archive suffix handling.

- Improve docker test skip detection.

- Event handling on newer kernels.

- Command line handler for the python extension.

- Linking against the swig generated symbol for the python extension.

- Linking on python 3.8 and newer.

- Regression in socket name shortening.

- Test caused by shell optimization.

- Test failure due to increased shebang limit.

- Handling of fstatat on new kernels.

- Seccomp event handling logic causing sysexit events to be missed.

- fake_id0: Fix POKE_MEM_ID to call poke_uint32 instead of poke_uint16.

5.2.0 - 2021-09-01
------------------

Added
~~~~~

-  GitLab CI/CD pipelines for static binaries.

-  Python extension.

-  Secure disclosure instructions.

-  Vagrantfiles for kernel-specific testing.

-  Support for Musl libc.

-  Use shellcheck for scripts.

-  link2symlink extension.

-  Contributor scripts care2docker.sh, and care_rearchiver.sh

-  Clang scan-build and gcov/lcov for source code analysis.

-  Trivial chroot using relative paths.

-  port_mapper extension.

-  Commandline option --kill-on-exit.

-  Hidden UVROOT_TMPDIR option.

-  Support for sudo via fake_id0 extension.

Changed
~~~~~~~

-  Started using top-level changelog instead of individual ones.

-  Limit testsuite to five minutes.

-  Updated release instructions.

-  Renamed tests to test.

-  Replace .exe file extension with .elf for loader binaries.

-  Use LC_ALL instead of LANG.

-  Semantics for HOST_PATH extension event arguments.

Removed
~~~~~~~

-  Disabled, deprecated, or unreliable tests.

-  Drop Coverity from Travis CI.

-  Cross-compiling scripts for Slackware.

-  FHS assumptions from tests.

-  References to uvroot.me domain.

Fixed
~~~~~

-  Error-code handling in substitute_binding_stat.

-  Prevent tracees from becoming undumpable.

-  Merged patches for detecting kernels >= 4.8.

-  GIT_VERSION for development binaries.

-  Replace mktemp with mkstemp.

-  File permissions for test scripts.

-  Filter renamteat2 syscall.

-  Honor GNU standards regarding DESTDIR variable.

-  Cleanup tmp on non-ext file systems.

-  Reallocation of heap for CLONE_VM on execve syscall.

-  Non-executable stack for binaries.

.. _Keep a Changelog: https://keepachangelog.com/en/1.0.0
.. _Semantic Versioning: https://semver.org/spec/v2.0.0.html
