# AI attribution / AI 参与声明

`uvroot` 的改版工作（重命名、功能整理、文档、缺陷修复与实测验证）由一个 AI
编码代理参与完成。按“开源项目标注 AI 参与”的通行做法，在此明确标识。

## 标识

| 项 | 值 |
|---|---|
| AI 标识名（你的名字） | **deepseek-flash** |
| 运行环境 | DeepSeek Harness (DSH) coding agent |
| 提交署名 | `Co-Authored-By: deepseek-flash <noreply@deepseek.com>` |
| 参与范围 | 见下 |

> 这份文件是**上游 PRoot 没有的**新增内容；上游代码本身的作者信息请见
> [`AUTHORS`](AUTHORS) 与 git 历史，未做任何改动。

## AI 具体做了什么

1. **改版重命名**：`PRoot` → `uvroot`（用户态虚拟根）
   - 二进制 `proot` → `uvroot`，源文件 `cli/proot.{c,h}` → `cli/uvroot.{c,h}`，
     `extension/python/proot.i` → `uvroot.i`，`doc/proot/` → `doc/uvroot/`
   - 环境变量 `PROOT_*` → `UVROOT_*`（共 24 个，未保留旧名兼容）
   - 运行时数据 `.proot-vperm` → `.uvroot-vperm`（上游旧名 `.proot-vperm`
     会被自动接管）
2. **文档**：`README.rst` 重写（上游署名/链接保留）、本文件、
   [`CHANGES-vs-PRoot.md`](CHANGES-vs-PRoot.md)
3. **缺陷修复**：`faccessat2`(439) 在 arm64/arm/i386/sh4 的 syscall 表缺失
   （上游 bug，会导致 ARM 上 glibc 容器里 `apt` 完全不可用）
4. **构建与测试脚本**：`test/` 下的 libcheck 单元测试、Bats 套件与 shellcheck
   job，以及编译期开关 `UVROOT_TMP_DIR_DEFAULT`
5. **隐私清理**：移除仓库内的私网 IP / 设备序列号 / 主机绝对路径
6. **虚拟网络（`--net`）**：新增 `src/extension/netvirt/`（设备/地址/路由模型、
   rtnetlink 与 `SIOCGIF*` 合成、`veth0`/`vtun` 映射、虚拟 `/dev/net/tun`、
   用户态 WireGuard 网桥与 NAT 回退）、CLI 选项、`doc/uvroot/manual.rst` 文档
7. **特权端口虚拟化（`-i 0:0`）**：新增 `src/extension/bindv/`（bind 端口
   改写、getsockname/loopback connect 还原、accept/dup 继承），配套
   `test/test-bindv.sh`
8. **隧道内 TCP 终止（`forward=`）**：新增 `src/extension/netvirt/wg/wg_tcp.c`
   （IPv4/TCP 服务端状态机、校验和、窗口、超时重传、本地 socket 中继），
   接入 `wg_engine.c` 的收发与 poll 循环；`fake_id0` 增加虚拟 root 的
   `chroot()` 放行；配套 `test/test-wg-tcp.sh` 与 `test/test-ssh-over-wg.sh`
   （后者用 paramiko 作为容器内 SSH 服务，因为 sshd 的 privsep 在假 root 下
   无法完成认证）
9. **隧道内 UDP 转发（`forward_udp=`）**：新增
   `src/extension/netvirt/wg/wg_udp.c`（数据报解析、每客户端 socket 映射、
   UDP 校验和、空闲回收），接入引擎收发与 poll；配套 `test/test-https-dns.sh`
   验证 HTTPS(443)、DNS over UDP/TCP(53)
10. **虚拟进程 id（`--vpid`）**：新增 `src/extension/vpid/`（pid 映射表、
   getpid/fork/wait/kill 翻译、/proc 路径与目录项替换、status/stat 生成），
   CLI 选项、文档与 `test/test-vpid.sh` 实测用例
8. **用户态 WireGuard 引擎**：`src/extension/netvirt/wg/`（BLAKE2s/HMAC/KDF、
   Noise IK 握手、传输数据面、UDP/tun 引擎），已实测与 Linux 内核 WireGuard
   双向互通；配套用例 `test/test-wg-crypto.sh`、`test/test-wg.sh`、
   `test/test-wg-kernel.sh` 与 `test/netvirt-*.c`、`test/wg-*.c`

## 人类作者需要确认的

* 若项目另有指定署名，请替换上表的 AI 标识名。
* AI 生成的代码请在合并前自行 review；AI 不承担任何担保责任，
  许可证与免责声明以 [`COPYING`](COPYING)（GPL-2.0-or-later）为准。
