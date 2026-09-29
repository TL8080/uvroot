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
   [`CHANGES-vs-PRoot.md`](CHANGES-vs-PRoot.md)、Android/NDK 实测报告
   `test/android-a5/README.md`
3. **缺陷修复**：`faccessat2`(439) 在 arm64/arm/i386/sh4 的 syscall 表缺失
   （上游 bug，会导致 ARM 上 glibc 容器里 `apt` 完全不可用）
4. **构建与测试脚本**：`test/android-a5/`（NDK 交叉编译、设备端 musl/glibc
   兼容性矩阵、zig 与 termux-glibc 工具链用例）
5. **隐私清理**：移除仓库内的私网 IP / 设备序列号 / 主机绝对路径

## 人类作者需要确认的

* 若项目另有指定署名，请替换上表的 AI 标识名。
* AI 生成的代码请在合并前自行 review；AI 不承担任何担保责任，
  许可证与免责声明以 [`COPYING`](COPYING)（GPL-2.0-or-later）为准。
