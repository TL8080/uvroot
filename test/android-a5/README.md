# Radxa A5 (Android 13 / arm64) — Termux + NDK + musl/glibc 容器兼容性实测

> 目标：把 USB 直连的安卓开发板当作真实目标机，用 USTC 镜像装好编译环境，
> 用 **Android NDK 交叉编译本仓库的 uvroot fork**，并在设备上实测它与
> **musl（Alpine）** 和 **glibc（Ubuntu）** 两种容器的兼容性。
>
> 结论先行：**musl 容器全绿；glibc 容器除“装包(dpkg 解包)”外全绿**；
> 过程中定位并修复了 **上游 PRoot 的 aarch64 `faccessat2` 缺失**（本仓库已改），
> 另有一个 **NDK 交叉编译产物特有的 tar 相对路径解包缺陷**尚未闭环。

---

## 1. 环境

| 项 | 值 |
|---|---|
| 主机 | Fedora 44 / x86_64，`adb` 37.0.0 |
| 设备 | **Radxa A5**（Allwinner A527），USB `1f3a:4ee7`，序列号 `<ADB_SERIAL>` |
| Android | 13（SDK 33），`userdebug`，fingerprint `Radxa/a527_radxa_a5_arm64/a527-radxa-a5:13/...` |
| 内核 | `5.15.119-aiot-t527-android13-v1.0` aarch64 |
| Root | Magisk（`uid=0 context=u:r:magisk:s0`），SELinux `Permissive` |
| Termux | `0.119.0-beta.3`，uid **10117**，`$PREFIX=/data/data/com.termux/files/usr` |
| 设备网络 | wlan0 `<BOARD_IP>`，可直连 USTC（~2.9 MB/s） |
| 设备存储 / 内存 | `/data` 11 GB（空闲 ~7 GB），RAM 2 GB |
| NDK | `~/Android/Sdk/ndk/27.2.12479018`（r27c，clang 18.0.3），target API 24 |

> 说明：本会话的 DSH 沙箱默认 `workspace-write` 会用精简 tmpfs 覆盖 `/dev`，
> 导致 `/dev/bus/usb` 不存在、adb 看不到设备；切到 `danger-full-access` 后正常。

---

## 2. 已完成的工作

### 2.1 切换 USTC 镜像

| 位置 | 文件 | 内容 |
|---|---|---|
| Termux | `$PREFIX/etc/apt/sources.list` | `deb https://mirrors.ustc.edu.cn/termux/apt/termux-main/ stable main` |
| Alpine 容器 | `/etc/apk/repositories` | `https://mirrors.ustc.edu.cn/alpine/v3.24/{main,community}` |
| Ubuntu 容器 | `/etc/apt/sources.list.d/ubuntu.sources` | `URIs: https://mirrors.ustc.edu.cn/ubuntu-ports/`（noble / -updates / -backports / -security） |

原文件备份为 `sources.list.orig`（Termux）。USTC 上 `termux-main`、`termux-root`、
`termux-x11`、`termux-main-21`、`alpine`、`ubuntu-ports`、`ubuntu-cdimage/ubuntu-base` 均可用。

### 2.2 安装编译环境（全部经 USTC）

```
clang 21.1.8      make 4.4.1        pkg-config 0.29.2   talloc 2.4.3
binutils          libtalloc(-static) git  curl  wget  tar  xz-utils  zip  unzip
proot-distro (python3.14)           openssh（随 proot-distro 带入）
zig 0.16.0                          gcc-glibc 14.2.1 + glibc 2.44 + binutils-glibc（termux-glibc）
```

`apt update` 走 USTC 成功；`apk update` 报 `OK: 28551 distinct packages available`。

### 2.3 从 adb 可靠进入 Termux 用户（关键脚手架）

`adb shell su 10117 -c …` 得到的进程是 **gid=0、groups=0**，缺少普通应用必有的
**`AID_INET(3003)`**，于是 Android netd 拒绝为其解析域名（`ping: unknown host`，
但直接 ping IP 正常）。用 NDK 现编了一个 12 行的权限落地器解决：

`test/android-a5/a5run.c` → `setgroups(3003, 9997, uid+10000, uid+40000)` +
`setgid/setuid` + 设 `PREFIX/HOME/PATH/LD_LIBRARY_PATH/TMPDIR` → `execvp`。

```bash
adb shell "su -c '/data/local/tmp/a5run /system/bin/ping -c1 mirrors.ustc.edu.cn'"   # 通
```

### 2.4 用 NDK 交叉编译本 fork

uvroot 唯一的硬依赖是 `talloc`，没有用“源码编译 talloc”的麻烦路线，而是直接取
**Termux 的 `libtalloc-static` (2.4.3, aarch64/Bionic)** 当 sysroot：

```
a5-test/sysroot/usr/{include/talloc.h, lib/libtalloc.a, lib/pkgconfig/talloc.pc}
```

再用一层 `CROSS_COMPILE=aarch64-linux-android24-*` 包装脚本喂给 `src/GNUmakefile`：

```bash
make -C src uvroot \
     CROSS_COMPILE=<a5-test>/toolchain/aarch64-linux-android24- \
     WITHOUT_PYTHON=1
```

产物：`ELF 64-bit LSB pie, ARM aarch64, interpreter /system/bin/linker64, for Android 24`，
stripped **350 KB**，`NEEDED` 只有 `libdl.so` + `libc.so`（talloc 已静态链入）。
构建仅 1 条告警（`loader/assembly-arm64.h` 的 `SP` clobber，属已知良性问题）。

> 交叉编译的坑：`make -f /path/GNUmakefile` 的 out-of-tree 构建会被 VPATH 里的
> 现成 x86_64 `src/uvroot` 命中而“已是最新”，必须在干净的源码副本里 in-tree 构建。

---

## 3. 兼容性矩阵（本 fork，NDK 构建，已含 §4.1 修复）

`P=uvroot-ndk`；调用形如
`uvroot -r <rootfs> -i 0:0 -w / -b /dev -b /proc -b /sys <cmd>`，
并设 `UVROOT_TMP_DIR=$PREFIX/tmp`。

| 测试项 | musl / Alpine 3.24 | glibc / Ubuntu 24.04 |
|---|---|---|
| 启动、`/etc/os-release`、`uname -m` | ✅ | ✅ |
| 动态加载器（`ld-musl-aarch64.so.1` / `ld-linux-aarch64.so.1`） | ✅ | ✅ |
| `busybox` / GNU coreutils 运行 | ✅ | ✅ |
| fork / exec / 管道 / `sort` | ✅ | ✅ |
| 文件 I/O + `stat`（mode/size） | ✅ | ✅ |
| 信号（`trap` + `kill -TERM $$`） | ✅ | ✅ |
| `/proc`、`/dev`、`/sys` 绑定 | ✅ | ✅ |
| 假 root（`-i 0:0` 下 `id`=0，可写 `/etc`） | ✅ | ✅ |
| 容器内 DNS（guest libc 读 `/etc/resolv.conf`） | ✅ | ✅ |
| 包管理器联网 | ✅ `apk update`（HTTPS，28551 包） | ✅ `apt-get update`（HTTPS + GPG 通过） |
| **容器内编译** | ✅ gcc 15.2.0 编译并运行 syscall 探针 | ✅ gcc 13.3.0 编译并运行 syscall 探针 |
| **容器内装包（`apk add` / `apt-get install`）** | ✅ `build-base` 正常 | ❌ **见 §4.2** |
| CLI：上游 `-0/--root-id` | ❌ 未知选项（见 §5） | ❌ |
| CLI：`--change-id`（proot-distro v4 用） | ✅ | ✅ |

对照：**Termux 官方 `proot` 包（NDK r30 构建的上游 PRoot）在同一 rootfs、同一参数下全部通过**，
包括 `apt-get install`；因此上表两处 ❌ 都不是 rootfs 或用法问题。

syscall 探针（`test/android-a5/probe.c`，静态编译）在两容器中均输出：

```
hello: pid=… uid=0 gid=0
child-exec-ok
stat: size=4 mode=0644
syscall-probe: PASS
```

### 3.1 另一条路线：在 Termux 里直接产出 musl/glibc 测试二进制（zig + termux-glibc）

不经过容器里的包管理器，也就绕开了 §4.2。两条互补的路子，都实测通过：

**(a) `zig cc` 交叉到 musl / glibc**（Termux `zig` 0.16.0）：

```sh
zig cc -target aarch64-linux-musl            -O2 probe.c   # 默认就是静态 musl
zig cc -target aarch64-linux-musl     -static -O2 probe.c
zig cc -target aarch64-linux-gnu.2.39         -O2 probe.c   # 动态 glibc，interp=/lib/ld-linux-aarch64.so.1
```

> `zig cc -target aarch64-linux-gnu*` **不支持 `-static`**：
> `error: libc of the specified target requires dynamic linking`。

**(b) `termux-glibc` 的 glibc 库 + gcc**（`gcc-glibc` 14.2.1 / glibc 2.44）：

```sh
apt install glibc-repo            # 加 deb https://packages.termux.dev/apt/termux-glibc/ glibc stable
apt update && apt install gcc-glibc binutils-glibc glibc-runner
env -u LD_LIBRARY_PATH $PREFIX/glibc/bin/aarch64-linux-gnu-gcc -O2 probe.c
```

> 该子仓库**只有官方源**（`packages.termux.dev` / `...-cf...`）；
> USTC / TUNA / BFSU / NJU 的 `/termux/apt/` 下都 **404**。

**两个坑（实测）**：

1. **必须去掉 `LD_LIBRARY_PATH=$PREFIX/lib`**，否则 glibc 驱动起不来：
   `cc1: error while loading shared libraries: .../glibc/lib/libc.so: invalid ELF header`
   —— `libc.so` 是 GNU ld 链接脚本（文本），不是 ELF。用 `env -u LD_LIBRARY_PATH`。
2. **动态产物的解释器指向 Termux 绝对路径**（`$PREFIX/glibc/lib/ld-linux-aarch64.so.1`），
   直接扔进容器会 `No such file or directory`。用自带的 `patchelf` 改一下即可：

   ```sh
   patchelf --set-interpreter /lib/ld-linux-aarch64.so.1 pg-termux
   ```

**结果**（Ubuntu 24.04 rootfs，glibc 2.39；同一批二进制）：

| 二进制 | 来源 | 需要的 glibc 版本 | FORK(修复) | CONTROL(官方 proot) |
|---|---|---|---|---|
| `probe-musl-static` | zig musl | — | ✅ | ✅ |
| `probe-glibc-dyn` | zig gnu.2.39（动态） | ≤2.39 | ✅ | ✅ |
| `probe-glibc-termux` | Termux gcc-glibc 14.2.1（动态） | 2.17/2.33/2.34 | ✅ | ✅ |
| `probe-glibc-termux-static` | 同上 `-static` | — | ✅ | ✅ |

`ldd` 在 guest 内正确解析：`libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6`。
把 musl 二进制放进 Ubuntu rootfs、把 glibc 二进制放进 Alpine rootfs 则**按预期失败**
（缺对应 loader），说明这组用例确实在考 libc，而非“碰巧静态能跑”。

结论：**用这套工具链产出的 musl 与 glibc 二进制，在本 fork 与官方 proot 下行为一致，全部通过。**
§4.2 的问题只出在“容器内装包（dpkg→tar 相对路径解包）”这条路径上。

### 3.2 虚拟用户（vperm）与读写隔离（--read-only / --ro）实测

板子上另跑了四组隔离用例，共 **135 项全绿**（musl 与 glibc guest 都覆盖）：

| 用例 | 覆盖 | 结果 |
|---|---|---|
| `cases/61-vperm-device.sh` | 虚拟 uid/gid/mode 数据库、宿主元数据不变、权限放行/拒绝、祖先 x 与父目录 w、`/etc/passwd` 读写、su/sudo 虚拟身份、shim 保护、setuid 族、跨 id 杀进程、DB 自保护与维护 | **43/43** |
| `cases/62-readonly-device.sh` | `--ro=<path>` 单绑定锁定、递归与子进程、`--ro=/`、`--read-only`（write/mkdir/unlink/truncate 全拒而读/stat/exec 正常）、id0 也拒、缺失路径仅告警、外部删除后不可重建、不跨运行泄漏 | **32/32** |
| `cases/63-isolation-bothguests.sh` | 上面两组的核心项在 **Alpine/musl 与 Ubuntu/glibc 两个 guest** 各跑一遍 | **24/24**（12+12） |
| `cases/64-vperm-db-protection.sh` | **数据库容器内只读性**：读写/追加/截断/删除/重命名(含落到 DB 上)/chmod/chown/等价路径(`/./`、`../`)、符号链接与**硬链接**绕过、`.tmp` 符号链接攻击；以及两条合法写入路径（宿主侧改库生效、uvroot 自身维护落库含换 inode 后仍受保护） | **36/36** |

关键证据（宿主侧独立核对，不由 guest 自证）：

```
# guest 内 chown/chmod 之后
guest stat: 700 1234:5678          <- vperm 数据库里的虚拟值
host  stat: 644 10117:10117        <- 宿主元数据一个字节都没动
```

访问控制与进程隔离（同样实测）：

| 场景 | 结果 |
|---|---|
| 非属主（id 1000）读 0700 文件 | `Permission denied` |
| 属主（id 1234）/ 虚拟 root（0:0） | 读到内容 |
| 虚拟 root 下 `sleep` 子进程，被降到 id 1000 的进程 `kill -9` | 拒绝（`setid` 返回 EPERM），进程存活 |
| 同 id 杀自己 | 允许 |
| `--read-only` 下 id0 写 | 拒绝（`--ro`/`--read-only` 对任何身份一视同仁）|
| 被 `--ro` 锁定的路径从宿主侧删除后，容器内重建 | 拒绝，且文件确实没回来 |

> 设备侧适配要点（写用例时踩到的坑，已固化进脚本）：
> 1. **guest 的 `PATH` 必须显式设置**——继承来的是 Termux 的 `PATH`，在容器里指向
>    不存在的 `/data/data/...`，busybox 会 `chmod: not found`。
> 2. 宿主版 `test-vperm.sh` 把宿主 `/bin,/usr,/lib,/etc` 绑进空 rootfs，Android 上不可用
>    （宿主是 Android 根），改用 **Alpine minirootfs 副本**做 guest。
> 3. Alpine 的 `id` 在 **`/usr/bin/id`**（不是 `/bin/id`）；`/bin/sh` 是指向
>    `/bin/busybox` 的**绝对符号链接**，所以 `[ -x ]`/`[ -e ]` 会在宿主侧解析失败，
>    判断 guest 可用性要 `[ -e ] || [ -L ]`。
> 4. 后台 `sleep` 必须重定向 fd，否则它会攥住命令替换的管道导致结果丢失（曾误报
>    “跨 id 杀进程没有被拒绝”）。
> 5. `test-image-guard.sh` 的 `--ro` 部分用 ext4 镜像 + `debugfs`，板子上没有 loop/ext4
>    工具链，改用**普通目录 + `-b` 绑定**，并从宿主侧核对文件是否真的没被创建。

---

## 4. 发现的问题

### 4.1 【已修复】aarch64(及 arm/i386/sh4) 缺 `PR_faccessat2` 映射 → 容器内 `apt` 全挂

**症状**：Ubuntu 24.04 里 `apt-get update` 对**所有**仓库报
`NO_PUBKEY 871920D1991BC93C`；`apt-key` 静默地把 keyring 换成 `/dev/null`。

**定位链路**（每一步都是实测，不是猜）：

1. `gpgv --keyring ubuntu-archive-keyring.gpg InRelease` 在**两个 proot 下都「Good signature」**
   → 不是 keyring、不是 gpgv、不是网络。
2. `apt-key` 的 `sh -x` 跟踪显示：`create_new_keyring` 里
   `[ ! -r /usr/share/keyrings/ubuntu-archive-keyring.gpg ]` **判定为真**（文件其实 0644 可读），
   于是 `TRUSTEDFILE=/dev/null` → `gpgv --keyring /dev/null`。
3. 自写 C 探针（`acctest.c`）打 errno：
   - `access(path, R_OK)` = **0 ok**
   - `faccessat(AT_FDCWD, path, R_OK, 0)` = **0 ok**
   - `faccessat(AT_FDCWD, path, R_OK, AT_EACCESS)` = **ENOENT**（未绑定路径）
     / **EROFS**（`/bin/ls`、`/etc/passwd` 这类路径）

   `EROFS`/`ENOENT` 的组合暴露了真相：**`/bin/ls`、`/etc/passwd` 是宿主 Android 上的真实路径**
   （`/bin → /system/bin`，只读分区），说明 **`faccessat2` 的 path 根本没被 uvroot 翻译**，
   直接透传给了内核。
4. 源码核对：`src/syscall/sysnums-arm64.h` 只有 `[48] = PR_faccessat`，
   **没有 `[439] = PR_faccessat2`**；`sysnums-x86_64.h` / `sysnums-x32.h` 有。
   缺映射 → `get_sysnum()` 认不出 439 → 不翻译路径。
   glibc ≥2.27 与 musl 在带 flags 时走 `faccessat2`，shell 的 `test -r` / `[ -r ]`
   也走它，于是 `apt-key` 的 keyring 可读性判断被误判。

**这是上游 bug，不是本 fork 引入的**：用同一套 NDK 工具链编译
`git archive HEAD`（上游 5.4.1-24-g2265984）后，同样复现。

**修复**（本仓库已改，4 个文件各加一行）：

```c
[439] = PR_faccessat2,
```

`src/syscall/sysnums-{arm64,arm,i386,sh4}.h`（x86_64/x32 本来就有；`faccessat2` 在
所有架构上都是 439）。

**验证**：修复后同一探针 `faccessat(..., AT_EACCESS)` 全部 `0 ok`；
Ubuntu 24.04 的 `apt-get update` 四个仓库全部 `Hit`（GPG 通过）。

### 4.2 【未闭环】tar / dpkg 的「相对当前目录」解包失败（NDK 构建产物特有）

**最小复现**（guest 内）：

```sh
mkdir -p /tmp/src/a/b/c && echo x > /tmp/src/a/f
cd /tmp/src && tar cf /tmp/m.tar ./a         # 成员名是 ./a/ ./a/b/ ./a/b/c/ ./a/f
rm -rf /tmp/dst && mkdir -p /tmp/dst && cd /tmp/dst
tar xf /tmp/m.tar
#   tar: ./a/b: Cannot mkdir: No such file or directory
#   结果只创建出 /tmp/dst/a
tar xf /tmp/m.tar -C /tmp/dst2                # 带 -C 就完全正常（5 项）
```

**影响**：`dpkg-deb -x` 正是以 cwd 相对方式调 tar 解包 `.deb`，
所以 **glibc 容器里 `apt-get install` / `dpkg -i` 必然失败**
（`tar: ./etc/ssl: Cannot mkdir: No such file or directory`，openssl 包 634 行错误）。
`apk` 正常（Alpine 走自己的解包路径）。

**已排除**：

| 假设 | 结论 |
|---|---|
| fork 的 `vperm` / `read-only` / 新扩展 | ❌ 上游 HEAD 同工具链编译后**同样复现** |
| 我的 `faccessat2` 修复引起 | ❌ 修复前后错误完全一致（633 行 vs 633 行） |
| 假 root / 身份证选项 | ❌ `-i 0:0` / 无 id / `-S` / `--vperm-id` 全部一样 |
| seccomp 加速 | ❌ `UVROOT_NO_SECCOMP=1` 无效 |
| `HAVE_PROCESS_VM`（交叉编译的 feature check 只判文件存在、没真跑） | ❌ 关掉重编，仍复现 |
| tar 本身 / symlink / `mkdirat`+dirfd 原语 | ❌ 都正常（`dirprobe.c` 全过） |
| cwd 跟踪、`./` 前缀、`/` 相对路径 | ❌ shell 手写同样序列全部成功 |

**已确认的边界**：uvroot `-v 2` 里 **tar 的 path 翻译日志缺失或异常**，
`mkdir("./a")` 后 `mkdir("./a/b")` 却拿到 ENOENT；而 **Termux 官方 proot 正常**。
两者差异只剩“构建方式”这一层（NDK **r27c/API 24** vs Termux 的 **NDK r30**），
所以怀疑是 NDK 版本/工具链相关的上游行为，需要下一步对照 Termux 的
`uvroot` 构建配方与其补丁。**建议优先级高**——它直接决定 glibc 容器能否装包。

### 4.3 【已修复】vperm 数据库在容器内可被**硬链接**读写 + `<db>.tmp` 符号链接可重定向 uvroot 的写

写 §3.2 的隔离用例时顺手做了攻击面探测，发现两个**真实可复现**的绕过：

**(a) 硬链接绕过（提权：容器内可自行改权限库）**

DB 的保护只比较**路径字符串**（`relative_of()` 里 `strcmp(rel, root->db_rel)`），
所以容器内建一个硬链接就能绕开：

```
$ uvroot -r $R --vperm-id=0 /bin/ln /.uvroot-vperm /t/dblink     # 成功
$ uvroot -r $R --vperm-id=0 /bin/cat /t/dblink
# uvroot vperm database - virtual ownership and permissions      <- 读到了
100700 10117 10117	t/data.txt
$ uvroot -r $R --vperm-id=0 /bin/sh -c 'echo "999999 0 0 pwned" >> /t/dblink'
  db md5 变了、size 90 -> 110                                     <- 写进去了
```

容器内因此可以给自己发任意 uid/gid、把 0700 改成 0777——**虚拟权限形同虚设**。

**(b) `<db>.tmp` 符号链接攻击（容器内 → 宿主任意文件写）**

`db_save()` 是 `fopen("<db>.tmp", "w")` 后 `rename()` 上去，而 `fopen` 会**跟随符号链接**。
`.uvroot-vperm.tmp` 不在保护名单里（只保护 `.uvroot-vperm`），所以容器内可以提前把它
做成指向宿主绝对路径的符号链接，让 uvroot 自己把内容写到那个文件上：

```
$ uvroot -r $R --vperm-id=0 /bin/ln -s <HOSTPATH> /.uvroot-vperm.tmp
$ uvroot -r $R --vperm  /bin/sh -c 'chmod 705 /t/data.txt'   # 触发 db_save
  victim=[# uvroot vperm database - ... 100705 10117 10117 t/data.txt]   <- 被改写
```

注意 guest 给的**绝对**符号链接目标不被翻译，所以它命名的是宿主路径——这是一个
容器内到宿主文件写的原语。

**修复**（`src/extension/vperm/vperm.c`，已实测前后对比）：

| 改动 | 作用 |
|---|---|
| `VpermRoot` 增加 `db_dev/db_ino/db_id`，`root_db_ident()` 记录身份，`db_save()` 成功后刷新 | 因为 `db_save()` 用 `rename()` 落盘，**每次保存 inode 都会变**，身份必须跟着刷新 |
| `relative_of()` 增加 `lstat(host)` 的 (dev,ino) 比较（用 `lstat` 而非 `stat`，故意让「创建指向 DB 的符号链接」继续成功，因为经过它的访问会被路径比较拦下） | 关掉硬链接绕过 |
| rename 处理里对**目的地**也做 `relative_of()` 检查 | 关掉「把别的文件 rename 到 DB 上」 |
| `db_save()` 改为 `unlink(tmp)` + `open(O_CREAT\|O_EXCL\|O_NOFOLLOW, 0600)`（EEXIST 重试 3 次） | 关掉 `.tmp` 符号链接；顺带把 DB 宿主权限从 0644 收紧到 **0600** |

**前后对比证据**（同一个用例，`uvroot-prefix` = 修复前的 HEAD 构建 / `uvroot-ndk` = 修复后）：

```
############ uvroot-prefix ############
  hardlink: read=[100700 10117 10117	t/data.txt] ... write_took_effect=0
  .tmp symlink: victim=[# uvroot vperm database - ...]  => TRUNCATED (exploited)

############ uvroot-ndk ############
  hardlink: read=[] ... write_took_effect=0
  .tmp symlink: victim=[IMPORTANT-DATA]  => SAFE
```

回归：`cases/64-vperm-db-protection.sh` **36/36**，且 61/62/63 全部保持通过。

**仍然成立的合法写入路径**（用例里都验了）：宿主侧直接改数据库下一次运行即生效；
uvroot 自身的维护写入（chmod/chown 落库、rename 搬迁、unlink 删条目、启动校验写 `.bak`）
正常工作，且**换 inode 之后新库依然受保护**。`.bak` 只写不读，被篡改无影响。

### 4.4 【环境类，已处置】

| 问题 | 现象 | 处置 |
|---|---|---|
| `su 10117` 缺 `AID_INET` | 域名解析失败 | NDK 编 `a5run`（§2.3） |
| `/tmp` 在 Android 不存在 | `uvroot: can't create temporary file` | 必须设 `UVROOT_TMP_DIR`（上游行为，非 fork bug） |
| Ubuntu base 无 CA 包 | `apt` 报 `No system certificates available` | 先塞宿主 CA bundle，装好 `ca-certificates` 后恢复 HTTPS |
| Ubuntu base 无 ssl 证书 + 首轮 http 引导留下残包 | dpkg 半损坏 | 用**全新 rootfs**重来（`ubuntu2`） |

---

## 5. CLI 兼容性：`-0/--root-id` 被删除

本 fork 的 `cli/uvroot.h` 删掉了上游的 `--root-id` / `-0`（diff 里可见
`-static int handle_option_0(...)`、`-.name = "--root-id"`），改用 `--vperm-id`。
后果：

* 上游文档与**海量 Termux 教程/脚本**里的 `proot -0 …` 现在报
  `uvroot error: unknown option '-0'`；等价写法是 `-i 0:0`。
* **当前 `proot-distro` v4 不受影响**——它用的是 `--change-id=<uid>:<gid>`
  （`proot_distro/commands/login/proot_cmd.py:104`），本 fork 支持。

建议：保留 `-0` 作为 `-i 0:0` 的兼容别名，成本一行，收益是所有现存脚本。

### 5.1 二进制名：proot-distro 按名字找 `proot`

`proot-distro` 是在 `PATH` 里查找名为 **`proot`** 的可执行文件。把本 fork 当 drop-in
用时需要一个同名软链（`test/android-a5/cases/44-proot-distro.sh` 就是这么做的）：

```sh
mkdir -p ~/bin-ours && ln -sf ~/a5/uvroot-ndk ~/bin-ours/proot
PATH=~/bin-ours:$PATH proot-distro login alpine
```

uvroot 自身的参数（`--change-id` 等）与 proot-distro v4 兼容，见上。

---

## 6. 怎么复现

```bash
# 0) 主机侧：NDK 包一层 CROSS_COMPILE
a5-test/toolchain/aarch64-linux-android24-{gcc,ld,strip,objcopy,objdump,pkg-config}

# 1) 干净的源码副本里 in-tree 构建
make -C <copy>/src uvroot CROSS_COMPILE=<...>/toolchain/aarch64-linux-android24- WITHOUT_PYTHON=1
adb push src/uvroot /data/local/tmp/uvroot

# 2) 设备侧：以 Termux 用户运行（含 AID_INET）
a5-test/run_as_termux.sh <job.sh>        # 内部用 a5run + UVROOT_TMP_DIR

# 3) 关键单点复现
bash a5-test/…   # 见下表脚本
```

| 脚本 | 作用 |
|---|---|
| `tools/a5run.c` | NDK 权限落地器（修 DNS） |
| `acctest.c` | `access`/`faccessat(AT_ACCESS)`/`open`/`stat` errno 探针 → 抓 §4.1 |
| `probe.c` | fork/exec/IO/stat syscall 探针 |
| `dirprobe.c` | `mkdirat`/`openat`/`symlinkat`+dirfd 原语 |
| `43-fresh-glibc.sh` | 全新 Ubuntu rootfs + gcc 编译运行全流程 |
| `10-alpine-compile.sh` | Alpine `build-base` + 编译运行 |
| `39/40/41-*.sh` | §4.2 的排除实验（id/process_vm/verbose） |
| `44-uvrootdistro.sh` / `45-argform.sh` | 用本 fork 顶替 `uvroot` 跑 proot-distro；校验其参数形式 |
| `46/47-*.sh` | 装 zig 0.16，交叉 musl/glibc 并在两种 rootfs 下运行 |
| `48/49/50/51/52-*.sh` | termux-glibc 仓库 → gcc-glibc → `LD_LIBRARY_PATH` 坑 → patchelf 跑通 |
| `61-vperm-device.sh` | **虚拟用户 vperm** 43 项（§3.2） |
| `62-readonly-device.sh` | **读写隔离 `--read-only`/`--ro`** 32 项（§3.2） |
| `63-isolation-bothguests.sh` | 上面两组的核心项在 musl + glibc guest 各跑一遍，24 项（§3.2） |
| `64-vperm-db-protection.sh` | **vperm 数据库容器内只读性**（含硬链接绕过与 `.tmp` 符号链接攻击），36 项（§4.3） |

## 7. 结论

* **USTC 镜像 + 编译环境**：Termux / Alpine / Ubuntu 三处都已切到 USTC 并验证可用；
  Termux 端装齐 clang/make/pkg-config/talloc/proot-distro。
* **NDK 交叉编译**：本 fork 用 NDK r27c + Termux `libtalloc.a` 可干净交叉编译出
  aarch64 Android 可执行文件，设备上可直接运行。
* **musl 容器**：**全绿**（含容器内 gcc 编译运行、`apk` 装包）。
* **glibc 容器**：**除装包外全绿**（含容器内 gcc 编译运行、`apt-get update`）。
* **不依赖容器包管理器的独立验证**（§3.1）：zig 产出的 musl 二进制、zig gnu.2.39 与
  Termux `gcc-glibc` 产出的 glibc 动态/静态二进制，在对应 rootfs 下**全部通过**，
  且与官方 proot 行为一致 —— 说明 uvroot 对两种 libc 的加载/系统调用路径本身没问题。
* **隔离能力（§3.2）**：**虚拟用户与读写隔离在板子上 135 项全绿**，
  且 musl 与 glibc guest 结果一致。最关键的一条是**隔离是真实的**：
  guest 里 `chown`/`chmod` 之后，宿主侧 `stat` 仍是 `644 10117:10117`，
  虚拟身份只活在 `.uvroot-vperm` 数据库里；`--ro`/`--read-only` 连 id0 都拒，
  但读/`stat`/`exec` 不受影响。
* **权限库自身的安全性（§4.3）**：探测时发现并修掉了两个真实绕过——
  硬链接可以读写数据库（等于容器内自行提权）、`<db>.tmp` 符号链接可以让
  uvroot 把内容写到宿主任意文件。修复后 36 项专测全绿，且数据库宿主权限
  由 0644 收紧为 0600。
* **净收益**：修掉上游 **aarch64 `faccessat2` 缺失**（这会让 **所有** ARM PRoot 用户
  的 glibc 容器里 `apt` 直接不可用）；定位并卡住 **NDK 构建产物的 tar 相对路径解包缺陷**
  （决定 glibc 容器能否装包），留了最小复现与完整排除矩阵。
