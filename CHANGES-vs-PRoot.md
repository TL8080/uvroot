# uvroot：相对 PRoot 的改动与使用方法

> uvroot（用户态虚拟根）是 [PRoot](https://github.com/proot-me/proot) 的 fork：
> 同样用 `ptrace` 在无特权、无内核模块的前提下给进程一个虚拟根文件系统，
> 并在其上增加了**虚拟磁盘/网络后端、虚拟权限、只读根、单进程多容器**等能力。
> 许可证与上游一致：**GPL-2.0-or-later**（见 [`COPYING`](COPYING)）。

---

## 1. 一句话差异

| | 上游 PRoot | uvroot |
|---|---|---|
| 定位 | chroot / bind / binfmt_misc 替代 | 用户态**虚拟根**：再加虚拟磁盘、网络共享、虚拟权限 |
| 根文件系统来源 | 本地目录 | 本地目录 **+ 网络共享 + 磁盘镜像 + 远程块设备** |
| 权限 | fake root（`-0`） | fake root **+ 持久化虚拟权限数据库（vperm）** |
| 只读 | 无 | `--read-only` / `--ro=<path>` |
| 多容器 | 一进程一容器 | `--multi`：一进程多容器、共享同一镜像 |
| 命令名 | `proot` | **`uvroot`** |
| 环境变量 | `PROOT_*` | **`UVROOT_*`**（无旧名兼容） |

---

## 2. 新增功能

### 2.1 虚拟文件系统后端（netfs）

把远程目录或磁盘镜像挂进容器，**像本地目录一样读写**，并且**可以当容器根目录**
（FTP/SMB 因协议限制除外）：

| 选项 | 后端 | 说明 |
|---|---|---|
| `--netfs=guest:uri` | 通用入口 | 由 URI scheme 选择后端 |
| `--ftp=guest:uri` | FTP / FTPS / SFTP | `ftp://` `ftps://` `sftp://` |
| `--smb=guest:uri` | SMB/CIFS | `smb://` |
| `--nfs=guest:uri` | NFS | `nfs://` |
| `--iscsi=guest:uri` | iSCSI | `iscsi://host/iqn/lun`，块设备 |
| `--nbd=guest:uri` | NBD | `nbd://`，块设备 |
| `--img=guest:path` | ext2/3/4 镜像 | 本地镜像文件 |
| `--qcow2=guest:path` | qcow2 镜像 | 支持 reflink/嵌套识别 |

### 2.2 虚拟权限（vperm）

- `--vperm`：启用**持久化**虚拟 uid/gid/mode 数据库（`.uvroot-vperm`），
  容器内 `stat/chmod/chown` 只改数据库，**从不改宿主元数据**；
  `open/access/execve` 按虚拟权限校验。删除路径保留条目，路径重建时
  沿用条目里记录的虚拟属主、权限位取自本次创建；数据库损坏则拒绝启动；
  旧名 `.proot-vperm` 会被自动接管。
- `--vperm-id=uid:gid`：指定虚拟身份（替代上游的 `-0`）。
- `--vperm-map=path`：用户映射目录（`users.conf` + 生成的 `su`/`sudo` shim）。
- `--vperm-file=path`：把数据库放到容器根之外（netfs 根必需）。
- `--vperm-nosu`：关闭 su/sudo 虚拟映射。

### 2.3 只读根

- `--read-only`：整个根只读。
- `--ro=<path>`：按路径只读，可重复；对 `id0` 也拒绝；
  运行中动态出现的路径即锁定；`stat/open/execve` 仍正常。

### 2.4 单进程多容器

```sh
uvroot --multi <容器1 参数> -- <容器2 参数> -- ...
```

同一个进程里跑多个容器，共享同一磁盘镜像/后端驱动（省内存、省连接）。

### 2.5 `/proc` 与 `df` 存储信息虚拟化

容器内 `df`/`statfs` 按根的类型（hostfs / netfs / imgfs / nblockfs）返回**真实**
容量与已用空间，而不是宿主的值；PRoot 内部绑定（vperm shim、注入的
`resolv.conf`）会从 `df` 里过滤掉。

---

## 3. 修复的缺陷

| 缺陷 | 影响 | 状态 |
|---|---|---|
| `faccessat2`(439) 未映射到 `PR_faccessat2`（arm64/arm/i386/sh4） | ARM 上该 syscall 的路径**完全不翻译**，`test -r` 失真 → **glibc 容器里 `apt-get update` 全仓库 `NO_PUBKEY`**；上游 HEAD 同样存在 | **已修**（`src/syscall/sysnums-*.h`） |

> 已知未闭环：`tar` **不带 `-C`** 的相对路径解包在 NDK 构建产物上失败
> （`mkdir("./a/b")` 返回 ENOENT），导致 `dpkg`/`apt-get install` 装包失败。
> 上游 PRoot 用同一套 NDK 工具链编译也复现，Termux 官方 proot 包不复现。

---

## 4. 行为变更（不向后兼容）

- **命令名**：`proot` → `uvroot`
- **环境变量**：`PROOT_TMP_DIR` → `UVROOT_TMP_DIR`，`PROOT_NO_SECCOMP` →
  `UVROOT_NO_SECCOMP`，`PROOT_NETFS_*` → `UVROOT_NETFS_*` ……共 24 个，**不保留旧名**。
- **运行时数据**：数据库文件名为 `.uvroot-vperm`；rootfs 里遗留的
  `.proot-vperm` 会被自动迁移过来，
  `$XDG_DATA_HOME/proot/vperm` → `$XDG_DATA_HOME/uvroot/vperm`。
- **`-0` / `--root-id` 不存在**：等价写法 `-i 0:0` 或 `--vperm-id=0:0`。
  注意 `proot-distro` 按名字在 `PATH` 里找 `proot`，当 drop-in 用需要软链（见 §6.4）。

---

## 5. 构建

```sh
# 依赖：libtalloc、libarchive（CARE 用）、uthash（CARE 用）
make -C src loader.elf loader-m32.elf build.h
make -C src uvroot care
make -C test            # 测试套件
```

没有 `/tmp` 的构建/运行环境（Android 等）需要：

```sh
export UVROOT_TMP_DIR=/data/data/com.termux/files/usr/tmp
```

**交叉编译**（把 ``CC`` 换成目标工具链即可）：

```sh
make -C src uvroot CC=aarch64-linux-android24-clang \
  CPPFLAGS='-DUVROOT_TMP_DIR_DEFAULT="/data/data/com.termux/files/usr/tmp"'
# -> src/uvroot  (aarch64 Android PIE, 仅依赖 libdl/libc)
```

---

## 6. 使用方法

### 6.1 最快上手

```sh
# Alpine（musl）rootfs
curl -LO https://mirrors.ustc.edu.cn/alpine/latest-stable/releases/aarch64/alpine-minirootfs-3.24.2-aarch64.tar.gz
mkdir -p ~/alpine && tar -xzf alpine-minirootfs-*-aarch64.tar.gz -C ~/alpine

uvroot -r ~/alpine -i 0:0 -w / -b /dev -b /proc -b /sys /bin/sh
```

> `-b /proc -b /dev -b /sys` 之后容器里才有 `/proc/self`、`/dev/null`；
> 也可以用别名 `-R <rootfs>`（`-r` + 推荐绑定）或 `-S <rootfs>`（再叠加假 root）。

### 6.2 常用组合

```sh
# 假 root + 推荐绑定
uvroot -R ~/alpine -i 0:0 /bin/sh

# 把宿主目录绑进容器
uvroot -r ~/ubuntu -i 0:0 -b ~/work:/work -w /work /bin/bash

# 强制竖屏/内核版本伪装（某些发行版要求）
uvroot -r ~/ubuntu -k 5.15.0 -i 0:0 /bin/bash

# 虚拟权限（持久化 uid/gid/mode，不动宿主元数据）
uvroot -r ~/alpine --vperm --vperm-id=0:0 /bin/sh

# 只读根（除指定路径外全锁）
uvroot -r ~/ubuntu -i 0:0 --read-only --ro=/tmp --ro=/run /bin/bash

# 网络共享当根 / 当挂载点
uvroot -r / --nfs=/mnt/n:'nfs://10.0.0.1/export?version=3' -i 0:0 /bin/sh
uvroot -r / --smb=/mnt/s:'smb://user:pass@host/share' -i 0:0 /bin/sh

# 本地镜像当根（强制 vperm）
uvroot --img=/:'img:/path/root.img' --vperm-id=0:0 -i 0:0 /bin/sh

# 远程块设备（iSCSI / NBD）
uvroot --iscsi=/:'iscsi://host/iqn.2026-01.example:disk/0' -i 0:0 /bin/sh
uvroot --nbd=/:'nbd://host:10809/export' -i 0:0 /bin/sh

# 一进程多容器（共享镜像）
uvroot --multi -r ~/alpine -i 0:0 /bin/sh -- -r ~/alpine -i 0:0 /bin/sh
```

> **块设备 / 远程根记得带 `-w /`**：不指定 `-w` 时，uvroot 会把宿主当前目录拿到
> guest 里做规范化；当根不是宿主上的某个目录（`--img=/:`、`--iscsi=/:`、`--nbd=/:`、
> `--qcow2=/:` 等）时该路径必然不存在，于是打印
> `can't chdir("…") in the guest rootfs` 并回退到 `/`。功能不受影响（容器照常
> 在 `/` 下启动），加 `-w /` 即可消除提示；目录根只要宿主 cwd 在 guest 内就不会有。

### 6.3 网络

容器**不虚拟化网络**：容器里跑的程序直接用宿主网络栈。
glibc/musl 会读容器内的 `/etc/resolv.conf`，所以 rootfs 里需要有它
（Alpine 精简镜像常缺失）：

```sh
printf 'nameserver 223.5.5.5\nnameserver 119.29.29.29\n' > ~/alpine/etc/resolv.conf
```

### 6.4 Android / Termux

```sh
export UVROOT_TMP_DIR=$PREFIX/tmp          # Android 没有 /tmp

# 从 adb 以 Termux 用户运行时要带 AID_INET(3003)，否则 netd 不给解析域名

# 当 proot-distro 的 drop-in：它按名字找 proot
mkdir -p ~/bin-ours && ln -sf ~/uvroot ~/bin-ours/proot
PATH=~/bin-ours:$PATH proot-distro login alpine
```

### 6.5 环境变量

| 变量 | 作用 |
|---|---|
| `UVROOT_TMP_DIR` | 内嵌 loader 的临时目录（Android 必需） |
| `UVROOT_NO_SECCOMP` | 关闭 seccomp 加速（纯 ptrace） |
| `UVROOT_IGNORE_MISSING_BINDINGS` | `-b` 源不存在时不报错 |
| `UVROOT_FORCE_KOMPAT` / `UVROOT_FORCE_FOREIGN_BINARY` | 强制兼容/外部二进制处理 |
| `UVROOT_NETFS_*` | 后端选择与调优（库路径 `UVROOT_NETFS_LIBCURL`、`_LIBSMBCLIENT`、`_LIBNFS`、`_LIBNBD`、`_LIBISCSI`、`_ZLIB`；调优 `_CACHE`、`_INSECURE`、`_ISCSI_INITIATOR`、`_NO_LOCK`、`_ALLOW_REMOTE_ROOT`、`_TAKEOVER`、`_SSH_KNOWN_HOSTS` …） |

---

## 7. 已知问题

1. **`tar` 相对路径解包**（见 §3）——影响 `dpkg`/`apt-get install`。
   绕法：`tar -C <dir>` 显式指定目标目录；或用 Termux 官方 `proot` 装包。
2. **可选后端需自行提供库**：构建时由 pkg-config 探测 `libcurl` / `smbclient` /
   `ext2fs` / `libnbd` / `libiscsi` / `libnfs` / `zlib`，缺少头文件时该后端编成 stub；
   运行时 `dlopen` 下列文件名（libcurl/libsmbclient/libnfs/libnbd/libiscsi 在设了
   `PREFIX` 时先试 `$PREFIX/lib/<name>.so`）：
   - FTP/FTPS/SFTP → **libcurl**：`libcurl.so.4`、`libcurl.so`（覆盖变量 `UVROOT_NETFS_LIBCURL`）
   - SMB/CIFS → **libsmbclient**：`libsmbclient.so.0`、`libsmbclient.so`（`UVROOT_NETFS_LIBSMBCLIENT`）
   - NFS → **libnfs**：`libnfs.so.16`、`libnfs.so`（`UVROOT_NETFS_LIBNFS`）
   - img/raw/file（ext2/3/4）→ **libext2fs**：`libext2fs.so.2`、`libext2fs.so`（无覆盖变量）
   - NBD → **libnbd**：`libnbd.so.0`、`libnbd.so`（`UVROOT_NETFS_LIBNBD`）
   - iSCSI → **libiscsi**：`libiscsi.so.11`、`libiscsi.so.0`、`libiscsi.so`（`UVROOT_NETFS_LIBISCSI`）
   - QCOW2 压缩簇 → **zlib**：`libz.so.1`、`libz.so`（`UVROOT_NETFS_ZLIB`）

   未探测到则对应 `--ftp/--smb/--nfs/--nbd/--iscsi/--img`（以及 QCOW2 压缩簇）不可用，
   其余功能不受影响；NDK 交叉编译默认**不带**这些库。
   Termux 包名：`libcurl`、`samba`（提供 libsmbclient）、`libnfs`、`e2fsprogs`（提供 libext2fs）、
   `zlib`；`libnbd` / `libiscsi` 官方源未打包。
3. **`-0/--root-id` 已移除**，现存脚本需改为 `-i 0:0`。

---

## 8. 出处与署名

- 上游：https://github.com/proot-me/proot （GPL-2.0-or-later）
- 本 fork 的 AI 参与声明：[`AI-ATTRIBUTION.md`](AI-ATTRIBUTION.md)
