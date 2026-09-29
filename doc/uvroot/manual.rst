=======
 uvroot
=======

-------------------------------------------------------------------------
``chroot``, ``mount --bind``, and ``binfmt_misc`` without privilege/setup
-------------------------------------------------------------------------

:Date: 2026-09-07
:Version: 5.4.1
:Manual section: 1


Synopsis
========

**uvroot** [*option*] ... [*command*]


Description
===========

uvroot is a user-space implementation of ``chroot``, ``mount --bind``,
and ``binfmt_misc``.  This means that users don't need any privileges
or setup to do things like using an arbitrary directory as the new
root filesystem, making files accessible somewhere else in the
filesystem hierarchy, or executing programs built for another CPU
architecture transparently through QEMU user-mode.  Also, developers
can use uvroot as a generic Linux process instrumentation engine thanks
to its extension mechanism, see CARE_ for an example.  Technically
uvroot relies on ``ptrace``, an unprivileged system-call available in
every Linux kernel.

The new root file-system, a.k.a *guest rootfs*, typically contains a
Linux distribution.  By default uvroot confines the execution of
programs to the guest rootfs only, however users can use the built-in
*mount/bind* mechanism to access files and directories from the actual
root file-system, a.k.a *host rootfs*, just as if they were part of
the guest rootfs.

When the guest Linux distribution is made for a CPU architecture
incompatible with the host one, uvroot uses the CPU emulator QEMU
user-mode to execute transparently guest programs.  It's a convenient
way to develop, to build, and to validate any guest Linux packages
seamlessly on users' computer, just as if they were in a *native*
guest environment.  That way all of the cross-compilation issues are
avoided.

uvroot can also *mix* the execution of host programs and the execution
of guest programs emulated by QEMU user-mode.  This is useful to use
host equivalents of programs that are missing from the guest rootfs
and to speed up build-time by using cross-compilation tools or
CPU-independent programs, like interpreters.

It is worth noting that the guest kernel is never involved, regardless
of whether QEMU user-mode is used or not.  Technically, when guest
programs perform access to system resources, uvroot translates their
requests before sending them to the host kernel.  This means that
guest programs can use host resources (devices, network, ...) just as
if they were "normal" host programs.

.. _CARE: https://proot-me.github.io/care

Project Status Tags
====================
[ACTIVE] — The project is actively maintained or has recent contributions as of the documentation date.

[OUTDATED] — The project hasn’t been updated or contributed to for a long time and may be incompatible with current systems.

Options
=======

The command-line interface is composed of two parts: first uvroot's
options (optional), then the command to launch (``/bin/sh`` if not
specified).  This section describes the options supported by uvroot,
that is, the first part of its command-line interface.


Regular options
---------------

-r path, --rootfs=path
    Use *path* as the new guest root file-system, default is ``/``.

    The specified *path* typically contains a Linux distribution where
    all new programs will be confined.  The default rootfs is ``/``
    when none is specified, this makes sense when the bind mechanism
    is used to relocate host files and directories, see the ``-b``
    option and the ``Examples`` section for details.

    It is recommended to use the ``-R`` or ``-S`` options instead.

-b path, --bind=path, -m path, --mount=path
    Make the content of *path* accessible in the guest rootfs.

    This option makes any file or directory of the host rootfs
    accessible in the confined environment just as if it were part of
    the guest rootfs.  By default the host path is bound to the same
    path in the guest rootfs but users can specify any other location
    with the syntax: ``-b *host_path*:*guest_location*``.  If the
    guest location is a symbolic link, it is dereferenced to ensure
    the new content is accessible through all the symbolic links that
    point to the overlaid content.  In most cases this default
    behavior shouldn't be a problem, although it is possible to
    explicitly not dereference the guest location by appending it the
    ``!`` character: ``-b *host_path*:*guest_location!*``.

-q command, --qemu=command
    Execute guest programs through QEMU as specified by *command*.

    Each time a guest program is going to be executed, uvroot inserts
    the QEMU user-mode *command* in front of the initial request.
    That way, guest programs actually run on a virtual guest CPU
    emulated by QEMU user-mode.  The native execution of host programs
    is still effective and the whole host rootfs is bound to
    ``/host-rootfs`` in the guest environment.

-w path, --pwd=path, --cwd=path
    Set the initial working directory to *path*.

    Some programs expect to be launched from a given directory but do
    not perform any ``chdir`` by themselves.  This option avoids the
    need for running a shell and then entering the directory manually.

-v value, --verbose=value
    Set the level of debug information to *value*.

    The higher the integer *value* is, the more detailed debug
    information is printed to the standard error stream.  A negative
    *value* makes uvroot quiet except on fatal errors.

-V, --version, --about
    Print version, copyright, license and contact, then exit.

-h, --help, --usage
    Print the version and the command-line usage, then exit.


Extension options
-----------------

The following options enable built-in extensions.  Technically
developers can add their own features to uvroot or use it as a Linux
process instrumentation engine thanks to its extension mechanism, see
the sources for further details.

-k string, --kernel-release=string
    Make current kernel appear as kernel release *string*.

    If a program is run on a kernel older than the one expected by its
    GNU C library, the following error is reported: "FATAL: kernel too
    old".  To be able to run such programs, uvroot can emulate some of
    the features that are available in the kernel release specified by
    *string* but that are missing in the current kernel.

-i string, --change-id=string
    Make current user and group appear as *string* "uid:gid".

    This option makes the current user and group appear as *uid* and
    *gid*.  Likewise, files actually owned by the current user and
    group appear as if they were owned by *uid* and *gid* instead.
    Note that the ``-0`` option is the same as ``-i 0:0``.

-p string, --port=string
    Map ports to others with the syntax as *string* "port_in:port_out ...".

    This option makes uvroot intercept bind and connect system calls,
    and change the port they use. The port map is specified
    with the syntax: ``-b *port_in*:*port_out*``. For example,
    an application that runs a MySQL server binding to 5432 wants
    to cohabit with other similar application, but doesn't have an
    option to change its port. uvroot can be used here to modify
    this port: ``uvroot -p 5432:5433 myapplication``. With this command,
    the MySQL server will be bound to the port 5433.
    This command can be repeated multiple times to map multiple ports.

-n, --netcoop
    Activates the network cooperation mode.

    This option makes uvroot intercept bind() system calls and
    change the port they are binding to to 0. With this, the system will
    allocate an available port. Each time this is done, a new entry is added
    to the port mapping entries, so that corresponding connect() system calls
    use the same resulting port.

Network mount options
---------------------

The following options make a remote or image-backed resource readable
and writable in the guest as if it were a local directory.  Nothing is
mounted at the kernel level: uvroot mirrors the resource on demand into a
private cache directory and binds that directory into the guest, so no
privilege, kernel module or ``/dev/fuse`` access is required.  This also
works on unrooted Android/Termux systems.

File contents are fetched when a file is opened for reading and pushed
back when the last descriptor referring to it is closed; ``mkdir``,
``rmdir``, ``unlink``, ``rename``, ``symlink`` and ``link`` are
forwarded to the remote side as well.  The directory transports cannot
represent hard links and symbolic links, so those two are rejected with
``EPERM`` there; a block backend stores them inside the filesystem
carried by the disk, exactly like a real disk would.  The cache
location can be changed with the ``UVROOT_NETFS_CACHE`` environment
variable (a unique directory is created inside it) and self-signed
servers are accepted when ``UVROOT_NETFS_INSECURE`` is set;
``UVROOT_NETFS_SSH_KNOWN_HOSTS`` points at the ``known_hosts`` file used
for SFTP.

Block-level resources are translated in user space and interpreted by a
built-in ext2/3/4 driver, so the ownership and permission bits stored in
the filesystem inside the disk are the ones reported to and enforced on
the guest; no ``.uvroot-vperm`` database is used for them.  The virtual
permission layer is nevertheless enabled automatically for such a mount.
The optional libraries (libcurl, libsmbclient, libnfs, libnbd, libiscsi,
libext2fs, zlib) are resolved with ``dlopen()``; a backend whose library
is missing reports that it is not available in this build, and the others
still work.  Their location can be forced with ``UVROOT_NETFS_LIBCURL``,
``UVROOT_NETFS_LIBNFS``, ``UVROOT_NETFS_LIBNBD``, ``UVROOT_NETFS_LIBISCSI``
and ``UVROOT_NETFS_ZLIB``; ``UVROOT_NETFS_ISCSI_INITIATOR`` sets the iSCSI
initiator IQN and ``UVROOT_NETFS_FORCE_CUSTOM_IO`` forces raw images
through the generic block ``io_manager`` (a debugging aid).

The filesystem inside a block image has a single driver: the first
process that opens it.  Any other process that wants the same image
(another ``uvroot --img=...:disk.img``, possibly with another virtual
identity) connects to that owner and forwards its directory operations
over a Unix socket, so there is only one copy of the block and inode
bitmaps and concurrent reads and writes cannot corrupt the image.  The
service is named after the resolved image path and lives in
``$XDG_RUNTIME_DIR`` (or ``/tmp``), in a directory private to the user;
the permission rules above still apply, since the metadata queries go
through the same service.  When the owner exits the clients reconnect,
or take the image over themselves, so the virtual I/O does not stop
before every container is gone.  Images whose backing storage is already
in use are refused: the same file reached through another name, a file
living inside an already mounted image, and two files sharing disk
blocks (a reflink or a snapshot) are all detected.

--netfs=string
    Mount a remote or image-backed resource, e.g. *guest:uri*.

    This option makes the content of a remote (or image-backed) resource
    readable and writable in the guest as if it were a local directory.
    The syntax is *guest_directory*:*uri*; when the guest directory is
    omitted the resource is mounted under ``/mnt``.  The backend is
    selected from the URI scheme: ``ftp://``, ``ftps://`` (implicit TLS),
    ``ftpes://`` (explicit TLS) and ``sftp://`` are handled by libcurl,
    ``smb://`` and ``cifs://`` by libsmbclient, ``nfs://`` by libnfs,
    ``nbd://`` (and ``nbd+unix://``, ``nbds://``) by libnbd, ``iscsi://``
    by libiscsi, and ``img://``/``raw://``/``file://`` (raw image) and
    ``qcow2://`` (QCOW2 image) by the built-in image backends.

    ``ftp://``/``smb://``/``nfs://`` are **refused as the guest root**
    (``--ftp=/``): those protocols do not carry symlinks, ownership and
    the full permission bits faithfully enough for a whole distribution,
    and a single missing symlink breaks it (Alpine's
    ``/bin/sh -> /bin/busybox`` for instance).  They remain fully
    supported as data mounts under a real rootfs.  Block-level sources
    (``img``/``qcow2``/``iscsi``/``nbd``) carry a real filesystem and are
    the supported way to boot from remote storage.  Set
    ``UVROOT_NETFS_ALLOW_REMOTE_ROOT=1`` to bypass the check for a static
    rootfs.

--ftp=string
    Mount a FTP, FTPS or SFTP share, e.g. *guest:uri*.

    Alias for ``--netfs`` restricted to the ``ftp://``, ``ftps://``,
    ``ftpes://`` and ``sftp://`` schemes, for instance:
    ``--ftp=/mnt/pub:ftp://user:password@host/pub``.

--smb=string
    Mount a SMB/CIFS share, e.g. *guest:uri*.

    Alias for ``--netfs`` restricted to the ``smb://`` and ``cifs://``
    schemes, for instance:
    ``--smb=/mnt/share:smb://user:password@server/share``.

--nfs=string
    Mount a NFS export, e.g. *guest:uri*.

    Alias for ``--netfs`` restricted to the ``nfs://`` scheme, for
    instance ``--nfs=/mnt/pub:nfs://server/export``.  The URI is handed
    to libnfs, so its query arguments are supported:
    ``?version=4&nfsport=2049``, ``?uid=<n>&gid=<n>``, ``?sec=krb5`` and
    so on.  A bare ``server/export`` is read as ``nfs://``.  Like FTP and
    SMB this is a directory transport: symbolic links found on the server
    are mirrored and followed, but creating links and changing ownership
    remotely is not supported.  Requires libnfs at run time.

--iscsi=string
    Mount an iSCSI LUN, e.g. *guest:uri*.

    The URI is ``iscsi://[user:password@]host[:port]/target-iqn[/lun]``
    (``iscsi+tcp://`` is accepted too); the default port is 3260 and the
    default LUN is 0.  Credentials enable CHAP.  The LUN is read and
    written through libiscsi and interpreted by the ext2/3/4 driver.
    Requires libiscsi at run time.

--nbd=string
    Mount a NBD export, e.g. *guest:uri*.

    Any URI libnbd understands is accepted, for instance
    ``nbd://host:10809/export``, ``nbd+unix:///export?socket=/run/nbd.sock``
    or ``nbds://host/export`` for TLS.  A bare ``host[:port][/export]``
    is read as ``nbd://``.  Requires libnbd at run time.

--img=string
    Mount a raw disk image, e.g. *guest:path*.

    The image holds an ext2/3/4 filesystem that is interpreted by the
    built-in user-space driver; ``--raw`` and ``--file`` are aliases.
    The image may be read-only, in which case the mount is read-only too.
    Requires the libext2fs headers at build time.

--qcow2=string
    Mount a QCOW2 disk image, e.g. *guest:path*.

    The QCOW2 container is translated on the fly into a block device and
    interpreted by the ext2/3/4 driver.  Version 2 and 3 images without
    backing file, snapshot or encryption are supported, including
    compressed clusters (zlib is resolved with ``dlopen()``) and
    copy-on-write writes with refcount maintenance; ``--qcow`` and
    ``--qemu+img`` are aliases.

Virtual permission options
--------------------------

These options give a guest root its own persistent ownership and
permission model, independent of the host.  A small text database
records a virtual uid, gid and mode for a subset of the paths; the
``stat()`` family reports those values, ``chmod()`` and ``chown()`` only
update the database (the host metadata is never modified), and
``open()``, ``access()`` and ``execve()`` are checked against the
recorded mode.  Paths without an entry keep the host's own metadata.
The virtual identity is per process: the ``setuid()``/``setgid()`` family
only changes it (the host credentials are never touched), and it is
inherited across fork and exec.  Virtual ``su`` and ``sudo`` programs are
generated and bound into the container so that identities can actually be
switched there.  Switching is bounded, though: only the virtual root may
change the virtual identity, another id can only pass its own value back,
and an unprivileged id can never switch to root -- neither through the
shims nor by calling the internal switch path directly.  Without the
virtual ``su``/``sudo`` (``--vperm-nosu``) an unprivileged id cannot
switch at all.  ``capset``, ``ptrace`` and ``process_vm_readv``/
``process_vm_writev`` are intercepted as well.

Process control follows the same identities: an id other than the virtual
root may only signal processes of its own identity.  ``kill``, ``tkill``,
``tgkill``, ``rt_sigqueueinfo``, ``rt_tgsigqueueinfo`` and
``pidfd_send_signal`` (including process groups) are checked, because the
host kernel cannot tell the virtual identities apart -- every guest
process really runs as the same host user.

The database is loaded when the container starts; a database that cannot
be read or that contains an unexpected entry stops uvroot instead of
being silently ignored.  Removing a path keeps its entry, and creating
that path again refreshes the entry: the recorded virtual owner is kept
and the permission bits come from the creation.  ``unlink``, ``rmdir``
and ``rename`` keep the database in sync, and it is written back
atomically.  For a local rootfs it is stored as *root*/.uvroot-vperm; a
netfs root lives in a temporary cache, so its database is kept outside
of it.  A database left behind under a pre-rename name
(``.proot-vperm``) is taken over automatically.

--vperm-nosu
    Do not expose the virtual su/sudo programs.

    Also enables the feature.  The virtual ``/etc/passwd`` is still
    provided, but ``su`` and ``sudo`` are not injected, so the virtual
    identity cannot be changed from inside the container: it is then
    fixed by ``--vperm-id`` when the container is started.  The shipped
    ``su``/``sudo`` shims and the mapping area are read-only for the
    container, which cannot delete, overwrite or rename them.

--vperm-map=path
    Use *path* as the user mapping directory.

    Also enables the feature.  The directory holds ``users.conf``
    (*name*:*uid*:*gid*[:*shell*] lines, one per line) and the generated
    ``su``/``sudo`` shims, which are bound into the container so that
    ``su`` and ``sudo`` work there with the virtual identities.  Defaults
    to ``$XDG_DATA_HOME/uvroot/vperm`` (or
    ``~/.local/share/uvroot/vperm``).  The directory also holds the
    virtual ``/etc/passwd``, which the container can read and write;
    switches re-read it, so users added there can be used immediately.

--vperm-file=path
    Store the virtual permission database at *path*.

    Also enables the feature.  The path must be absolute.  It is mainly
    useful to keep the database outside a netfs root, where it would
    otherwise be mirrored to the server.

--read-only
    Mark every netfs mount of the container read-only (``--img``,
    ``--qcow2``, ``--netfs``, ``--ftp``, ``--smb``, ``--nfs``, ...), and
    every ``-b`` binding and the rootfs given by ``-r``.

    The refusal is enforced where the change would be applied, not by the
    virtual permission layer, so the virtual root cannot write either.
    ``--ro=path`` is the way to name a single path: it is a rule about
    paths, not about mounts -- *path* does not have to be a mount point,
    and everything below it (sub-directories and files) is read-only as
    well.  The path must exist when the container starts, otherwise the
    rule is ignored with a warning so the container can still create it.
    Once installed it stays in force even if the path is later removed or
    changed by the host or by another container; the path may then not be
    recreated from inside (``EPERM``), while a change below it gives
    ``EROFS``.  ``/dev``, ``/proc``, ``/sys`` and ``/run`` are never made
    read-only, so a shell can still use ``/dev/null``.

    ::

        uvroot -r / --img=/data:disk.img --ro=/data /bin/sh
        uvroot -r / -b /src:/src --ro=/src /bin/sh

--vperm-id=string
    Use *string* "uid:gid" as the virtual identity.

    Also enables the feature.  The identity is what permission checks
    and newly created files use; it defaults to the real uid and gid of
    the uvroot process.

--vperm
    Enable persistent virtual ownership and permissions.

    Uses the default database location (*root*/.uvroot-vperm for a local
    rootfs).

Alias options
-------------

The following options are aliases for handy sets of options.

-R path
    Alias: ``-r *path*`` + a couple of recommended ``-b``.

    Programs isolated in *path*, a guest rootfs, might still need to
    access information about the host system, as it is illustrated in
    the ``Examples`` section of the manual.  These host information
    are typically: user/group definition, network setup, run-time
    information, users' files, ...  On all Linux distributions, they
    all lie in a couple of host files and directories that are
    automatically bound by this option:

    * /etc/host.conf
    * /etc/hosts
    * /etc/hosts.equiv
    * /etc/mtab
    * /etc/netgroup
    * /etc/networks
    * /etc/passwd
    * /etc/group
    * /etc/nsswitch.conf
    * /etc/resolv.conf
    * /etc/localtime
    * /dev/
    * /sys/
    * /proc/
    * /tmp/
    * /run/
    * /var/run/dbus/system_bus_socket
    * $HOME
    * *path*

-S path
    Alias: ``-r *path*`` + a couple of recommended ``-b``; the guest runs
    as the (faked) root id.

    This option is useful to safely create and install packages into
    the guest rootfs.  It is similar to the ``-R`` option except it
    fakes the root identity and binds only the following minimal set
    of paths to avoid unexpected changes on host files:

    * /etc/host.conf
    * /etc/hosts
    * /etc/nsswitch.conf
    * /etc/resolv.conf
    * /dev/
    * /sys/
    * /proc/
    * /tmp/
    * /run/shm
    * $HOME
    * *path*


Exit Status
===========

If an internal error occurs, ``uvroot`` returns a non-zero exit status,
otherwise it returns the exit status of the last terminated
program. When an error has occurred, the only way to know if it comes
from the last terminated program or from ``uvroot`` itself is to have a
look at the error message.


Files
=====

uvroot reads links in ``/proc/<pid>/fd/`` to support `openat(2)`-like
syscalls made by the guest programs.


Examples
========

In the following examples the directories ``/mnt/slackware-8.0`` and
``/mnt/armslack-12.2/`` contain a Linux distribution respectively made
for x86 CPUs and ARM CPUs.


``chroot`` equivalent
---------------------

To execute a command inside a given Linux distribution, just give
``uvroot`` the path to the guest rootfs followed by the desired
command.  The example below executes the program ``cat`` to print the
content of a file::

    uvroot -r /mnt/slackware-8.0/ cat /etc/motd
    
    Welcome to Slackware Linux 8.0

The default command is ``/bin/sh`` when none is specified. Thus the
shortest way to confine an interactive shell and all its sub-programs
is::

    uvroot -r /mnt/slackware-8.0/
    
    $ cat /etc/motd
    Welcome to Slackware Linux 8.0


``mount --bind`` equivalent
---------------------------

The bind mechanism enables one to relocate files and directories.  This is
typically useful to trick programs that perform access to hard-coded
locations, like some installation scripts::

    uvroot -b /tmp/alternate_opt:/opt
    
    $ cd to/sources
    $ make install
    [...]
    install -m 755 prog "/opt/bin"
    [...] # prog is installed in "/tmp/alternate_opt/bin" actually

As shown in this example, it is possible to bind over files not even
owned by the user.  This can be used to *overlay* system configuration
files, for instance the DNS setting::

    ls -l /etc/hosts
    -rw-r--r-- 1 root root 675 Mar  4  2011 /etc/hosts

::

    uvroot -b ~/alternate_hosts:/etc/hosts
    
    $ echo '1.2.3.4 google.com' > /etc/hosts
    $ resolveip google.com
    IP address of google.com is 1.2.3.4
    $ echo '5.6.7.8 google.com' > /etc/hosts
    $ resolveip google.com
    IP address of google.com is 5.6.7.8

Another example: on most Linux distributions ``/bin/sh`` is a symbolic
link to ``/bin/bash``, whereas it points to ``/bin/dash`` on Debian
and Ubuntu.  As a consequence a ``#!/bin/sh`` script tested with Bash
might not work with Dash.  In this case, the binding mechanism of
uvroot can be used to set non-disruptively ``/bin/bash`` as the default
``/bin/sh`` on these two Linux distributions::

    uvroot -b /bin/bash:/bin/sh [...]

Because ``/bin/sh`` is initially a symbolic link to ``/bin/dash``, the
content of ``/bin/bash`` is actually bound over this latter::

    uvroot -b /bin/bash:/bin/sh
    
    $ md5sum /bin/sh
    089ed56cd74e63f461bef0fdfc2d159a  /bin/sh
    $ md5sum /bin/bash
    089ed56cd74e63f461bef0fdfc2d159a  /bin/bash
    $ md5sum /bin/dash
    089ed56cd74e63f461bef0fdfc2d159a  /bin/dash

In most cases this shouldn't be a problem, but it is still possible to
strictly bind ``/bin/bash`` over ``/bin/sh`` -- without dereferencing
it -- by specifying the ``!`` character at the end::

    uvroot -b '/bin/bash:/bin/sh!'
    
    $ md5sum /bin/sh
    089ed56cd74e63f461bef0fdfc2d159a  /bin/sh
    $ md5sum /bin/bash
    089ed56cd74e63f461bef0fdfc2d159a  /bin/bash
    $ md5sum /bin/dash
    c229085928dc19e8d9bd29fe88268504  /bin/dash


``chroot`` + ``mount --bind`` equivalent
----------------------------------------

The two features above can be combined to make any file from the host
rootfs accessible in the confined environment just as if it were
initially part of the guest rootfs.  It is sometimes required to run
programs that rely on some specific files::

    uvroot -r /mnt/slackware-8.0/
    
    $ ps -o tty,command
    Error, do this: mount -t proc none /proc

works better with::

    uvroot -r /mnt/slackware-8.0/ -b /proc
    
    $ ps -o tty,command
    TT       COMMAND
    ?        bash
    ?        uvroot -b /proc /mnt/slackware-8.0/
    ?        sh
    ?        ps -o tty,command

Actually there's a bunch of such specific files, that's why uvroot
provides the option ``-R`` to bind automatically a pre-defined list of
recommended paths::

    uvroot -R /mnt/slackware-8.0/
    
    $ ps -o tty,command
    TT       COMMAND
    pts/6    bash
    pts/6    uvroot -R /mnt/slackware-8.0/
    pts/6    sh
    pts/6    ps -o tty,command


``chroot`` + ``mount --bind`` + ``su`` equivalent
-------------------------------------------------

Some programs will not work correctly if they are not run by the
"root" user, this is typically the case with package managers.  uvroot
can fake the root identity and its privileges when the ``-0`` (zero)
option is specified::

    uvroot -r /mnt/slackware-8.0/ -0
    
    # id
    uid=0(root) gid=0(root) [...]
    
    # mkdir /tmp/foo
    # chmod a-rwx /tmp/foo
    # echo 'I bypass file-system permissions.' > /tmp/foo/bar
    # cat /tmp/foo/bar
    I bypass file-system permissions.

This option is typically required to create or install packages into
the guest rootfs.  Note it is *not* recommended to use the ``-R``
option when installing packages since they may try to update bound
system files, like ``/etc/group``.  Instead, it is recommended to use
the ``-S`` option.  This latter enables the ``-0`` option and binds
only paths that are known to not be updated by packages::

    uvroot -S /mnt/slackware-8.0/
    
    # installpkg perl.tgz
    Installing package perl...


``chroot`` + ``mount --bind`` + ``binfmt_misc`` equivalent
----------------------------------------------------------

uvroot uses QEMU user-mode to execute programs built for a CPU
architecture incompatible with the host one.  From users'
point-of-view, guest programs handled by QEMU user-mode are executed
transparently, that is, just like host programs.  To enable this
feature users just have to specify which instance of QEMU user-mode
they want to use with the option ``-q``::

    uvroot -R /mnt/armslack-12.2/ -q qemu-arm
    
    $ cat /etc/motd
    Welcome to ARMedSlack Linux 12.2

The parameter of the ``-q`` option is actually a whole QEMU user-mode
command, for instance to enable its GDB server on port 1234::

    uvroot -R /mnt/armslack-12.2/ -q "qemu-arm -g 1234" emacs

uvroot allows one to mix transparently the emulated execution of guest
programs and the native execution of host programs in the same
file-system namespace.  It's typically useful to extend the list of
available programs and to speed up build-time significantly.  This
mixed-execution feature is enabled by default when using QEMU
user-mode, and the content of the host rootfs is made accessible
through ``/host-rootfs``::

    uvroot -R /mnt/armslack-12.2/ -q qemu-arm
    
    $ file /bin/echo
    [...] ELF 32-bit LSB executable, ARM [...]
    $ /bin/echo 'Hello world!'
    Hello world!

    $ file /host-rootfs/bin/echo
    [...] ELF 64-bit LSB executable, x86-64 [...]
    $ /host-rootfs/bin/echo 'Hello mixed world!'
    Hello mixed world!

Since both host and guest programs use the guest rootfs as ``/``,
users may want to deactivate explicitly cross-filesystem support found
in most GNU cross-compilation tools.  For example with GCC configured
to cross-compile to the ARM target::

    uvroot -R /mnt/armslack-12.2/ -q qemu-arm
    
    $ export CC=/host-rootfs/opt/cross-tools/arm-linux/bin/gcc
    $ export CFLAGS="--sysroot=/"   # could be optional indeed
    $ ./configure; make

As with regular files, a host instance of a program can be bound over
its guest instance.  Here is an example where the guest binary of
``make`` is overlaid by the host one::

   uvroot -R /mnt/armslack-12.2/ -q qemu-arm -b /usr/bin/make
   
   $ which make
   /usr/bin/make
   $ make --version # overlaid
   GNU Make 3.82
   Built for x86_64-slackware-linux-gnu

It's worth mentioning that even when mixing the native execution of
host programs and the emulated execution of guest programs, they still
believe they are running in a native guest environment.  As a
demonstration, here is a partial output of a typical ``./configure``
script::

    checking whether the C compiler is a cross-compiler... no


Downloads
=========

uvroot
-----

The source code for uvroot and CARE are hosted in the same repository on `GitHub <https://github.com/proot-me/proot>`_.
Previous uvroot releases were packaged at https://github.com/proot-me/proot-static-build/releases, however, that
repository has since been archived. The latest builds can be found under the job artifacts for the `GitLab CI/CD Pipelines <https://gitlab.com/proot/proot/pipelines>`_ for each commit. The following commands can be used to download the latest x86_64 binary for convenience::

    curl -LO https://proot.gitlab.io/proot/bin/proot
    chmod +x ./uvroot
    uvroot --version

Rootfs
------

The following URLs contain rootfs archives that can be freely downloaded.
Note that ``mknod`` errors reported by ``tar`` when
extracting these archives can be safely ignored since special files
are typically bound (see ``-R`` option for details).

* https://download.openvz.org/template/precreated

* https://images.linuxcontainers.org/images

* http://distfiles.gentoo.org/releases

* http://cdimage.ubuntu.com/ubuntu-base

* https://archlinuxarm.org/about/downloads

* https://alpinelinux.org/downloads

Technically such rootfs archive can be created by running the
following command on the expected Linux distribution::

    tar --one-file-system --create --gzip --file my_rootfs.tar.gz /


Ecosystem
=========

The following ecosystem has developed around uvroot since it has been
made publicly available.

Projects using uvroot or CARE
----------------------------

* `ATOS
  <http://compilfr.ens-lyon.fr/wp-content/uploads/2013/12/17-Francois_DeFerriere.pdf>`_:
  find automatically C/C++ compiler options that provide best
  optimizations.

* CARE_: archive material used during an execution to make it
  reproducible on any Linux system.

* `[OUTDATED] Debian noroot
  <https://play.google.com/store/apps/details?id=com.cuntubuntu>`_:
  use Debian Linux on Android without root access.

* `[OUTDATED] GNURoot
  <https://github.com/corbinlc/GNURootDebian>`_:
  use several Linux distros on Android without root access.

* `JuNest <https://github.com/fsquillace/junest>`_:
  use Arch Linux on any Linux distros without root access.

* `[OUTDATED] OPAM2Debian <https://github.com/gildor478/opam2debian>`_:
  create Debian packages which contains a fully compiled OPAM
  installation.

* `OpenMOLE <https://github.com/openmole/openmole>`_:
  execute programs on distributed computing environments.

* `[OUTDATED] Polysquare Travis Container
  <https://github.com/polysquare/polysquare-travis-container>`_:
  use several Linux distros on Travis-CI without root access.

* `[OUTDATED] Portable PyPy <https://github.com/squeaky-pl/portable-pypy>`_:
  portable 32 and 64 bit x86 PyPy binaries.

* `SIO Workers <http://sioworkers.readthedocs.org/en/latest>`_:
  batch long-term computations with Python.


Third party packages
--------------------

Binaries from the Downloads_ section are likely more up-to-date.

* `Alpine Linux <https://pkgs.alpinelinux.org/packages?name=uvroot>`_

* `Arch Linux <https://aur.archlinux.org/packages/uvroot>`_

* `Debian <https://packages.debian.org/sid/uvroot>`_

* `Gentoo <http://packages.gentoo.org/package/sys-apps/uvroot>`_

* `NixOS <https://mynixos.com/nixpkgs/package/uvroot>`_

* `Termux <https://wiki.termux.com/wiki/uvroot>`_

* `Ubuntu <https://launchpad.net/ubuntu/+source/uvroot>`_

* `University of Chicago RCC <https://rcc.uchicago.edu/docs/software/modules/uvroot/midway2/current.html>`_

* `Void Linux <https://github.com/void-linux/void-packages/tree/master/srcpkgs/uvroot>`_


Public material about uvroot or CARE
-----------------------------------

* articles on `Rémi's blog
  <https://blog.duraffort.fr/tag/uvroot.html>`_.  Rémi (a.k.a Ivoire)
  is one of the uvroot developers.

* presentation "`Software engineering tools based on syscall
  instrumentation
  <https://archive.fosdem.org/2014/schedule/event/syscall>`_" during
  FOSDEM 2014.

* presentation "`SW testing & Reproducing a LAVA failures locally
  using CARE <https://connect.linaro.org/resources/lcu14/lcu14-211-lava-use-cases-sw-testing-reproducing-a-lava-failures-locally-using-care>`_"
  during Linaro Connect USA 2014

* presentation and essay "`CARE: the Comprehensive Archiver for
  Reproducible Execution
  <http://c-mind.org/events/trust2014/presentations/trust14_care.pdf>`_"
  (`essay <http://dl.acm.org/citation.cfm?doid=2618137.2618138>`_)
  during TRUST 2014

* presentation "`An Introduction to the CARE tool (dead link)
  <#>`_"
  during HiPEAC CSW 2013

* presentation and essay "`uvroot: a Step Forward for QEMU User-Mode
  <http://adt.cs.upb.de/quf/quf11/quf2011_13.pdf>`_" (`proceedings
  <http://adt.cs.upb.de/quf/quf2011_proceedings.pdf>`_) during
  QUF'11

* tutorial "`How to install nix in home (on another distribution)
  <https://wiki.nixos.org/wiki/Nix_Installation_Guide#uvroot>`_"


Companies using uvroot or CARE internally
----------------------------------------

* STMicroelectronics
* Sony
* Ericsson
* Cisco
* Gogo
* Infinite Omicron, LLC.


See Also
========

chroot(1), mount(8), binfmt_misc, ptrace(2), qemu(1), sb2(1),
bindfs(1), fakeroot(1), fakechroot(1)


Colophon
========

Visit https://proot-me.github.io for help, bug reports, suggestions, patches, ...
Copyright (C) 2023 uvroot Developers, licensed under GPL v2 or later.

::

     _____ _____              ___
    |  __ \  __ \_____  _____|   |_
    |   __/     /  _  \/  _  \    _|
    |__|  |__|__\_____/\_____/\____|

