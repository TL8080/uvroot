# 在 Termux + uvroot 隔离根里跑安卓应用（Radxa A5 / Android 13）

> 目标：不用 root、不改系统，用 **uvroot**（用户态虚拟根）给一个普通 Android
> 应用搭一个“只看得见被绑定路径”的隔离根，并在里面真的把应用的代码跑起来。
> 同时把**跑不起来的那部分（完整 GUI）**用系统侧证据钉死边界，而不是含糊带过。
>
> 被测应用：`com.matecal.ceiling`（吊顶材料计算器，uid 10114，含 Vulkan 原生库）。

---

## 1. 结论速览

| 能力 | 结果 | 证据 |
|---|---|---|
| 只含映射的隔离根（`ls /` 只有 8 项） | ✅ | `cases/65-app-isolation.sh` §2 |
| 未绑定路径在隔离根里**不存在**（`/sdcard`、`/etc`、其他应用数据…） | ✅ | 同上 |
| 隔离根里跑 ART（`/system/bin/app_process64`） | ✅ | §3 |
| 加载应用 dex、跑应用自己的计算引擎 | ✅ | `calcAll` 得 17.28 m² / 48 板 / 19.2 m 主龙骨 |
| 加载应用原生库 `libceilingvk.so` + `libc++_shared.so` | ✅ | §3 `dlopen` |
| **只**读到我们给的影子数据（真实数据一个字节没动） | ✅ | 影子内 `rooms.json` 与真实内容不同 |
| `--read-only` 拒绝写、仍可读 | ✅ | §5 |
| `-i 0:0` 假 root，宿主元数据不变 | ✅ | §6 |
| **零 `su`** 路径（adb shell uid 2000 全程跑通） | ✅ | `no-su-check.sh` **20/20** |
| **回测**：MT 管理器自己的 `readlink` 在隔离内看不到 `/sdcard` | ✅ | §7（`/storage/self/primary` → `null`） |
| `am start` 从隔离根拉起应用 UI | ❌ | §8：AMS 把 caller 包名写死成 `com.android.shell` |
| 直接 binder `startActivityAsUser` | ⚠️ 调用返回 0，但被 AMS 的**后台启动限制**拦下 | §8 logcat |
| 完整 GUI 跑在隔离根里 | ❌ **不可能**（无 root / 无自定义 zygote） | §8 |

三套脚本，总计 **94 项断言全绿**：

| 脚本 | 身份 | 结果 |
|---|---|---|
| `cases/65-app-isolation.sh`（吊顶计算器） | Termux（uid 10117） | **35/35** |
| `cases/66-mt-isolation.sh`（MT 管理器，A/B 回测） | Termux（uid 10117） | **39/39** |
| `app-isolation/no-su-device.sh` | adb shell（uid 2000，**无 `su`**） | **20/20** |

---

## 2. 隔离模型

隔离根是一个**空目录**，uvroot 只把下面这些绑进去。没绑的路径在 guest 里根本不存在：

```
uvroot -r <空目录> \
  -b /system -b /apex -b /vendor -b /linkerconfig \   # 系统库 + ART + 链接器配置
  -b <应用安装目录>:/data/app/<pkg> \                  # base.apk（dex + 资源）
  -b <影子目录>:/data/data/<pkg> \                     # 应用数据（我们自己的副本）
  -b <harness>:/opt/harness \                         # 探针 dex + 原生库
  -b /dev -b /proc -b /sys -w / <cmd...>
```

于是 guest 内的根目录**只有**：

```
apex  data  dev  linkerconfig  opt  proc  sys  system  vendor
```

`/data/data` 里**只有**目标应用一个目录；`/sdcard`、`/storage`、`/etc`、`/home`、
`/tmp`、`/root`、`/data/media`、`/data/data/com.termux` 全部 `absent`。

> 注意 `/linkerconfig` 必须绑，否则每条命令都会报
> `failed to find .../ld.config.txt`。
>
> guest 里的 `PATH` 不会自动带上 `/system/bin`：继承来的是 Termux 的 `PATH`。
> 用例里显式 `export PATH=$PREFIX/bin:...:/system/bin`，否则 `ls`/`id`/`cmd`
> 都会 `not found`（上一轮的 `/system/bin/am: cmd: inaccessible or not found`
> 就是这个原因，不是权限问题）。

---

## 3. 怎么复现

```bash
cd test/android-a5

# 一次性：编译两个探针并推到板子（需要 JDK 21 与 Android SDK build-tools）
ADB="adb -s <serial>" JAVA_HOME=~/tools/jdk-21.0.2 ANDROID_SDK=~/Android/Sdk \
  app-isolation/build-and-push.sh

# A. Termux 身份（uid 10117）跑隔离套件，并顺带抓 GUI 边界的系统侧证据
DEVICE=<serial> app-isolation/run-suite.sh

# B. 零 su 对照：全部以 adb shell（uid 2000）运行
DEVICE=<serial> app-isolation/no-su-check.sh
```

设备侧唯一的前置条件是 Termux 里已有 NDK 交叉编译出来的 uvroot
（`$HOME/a5/uvroot-ndk`）与 `a5run`（`/data/local/tmp/a5run`，见主 README §2.3）。

---

## 4. 应用真的在跑吗

`AppProbe` 用 `app_process64` 起 ART，classpath 直接给 APK，反射调用应用自己的
计算链路。以下是实测输出（`---` 后为说明）：

```
PROBE: uid=10117 10117 10117 10117
PROBE: java.home=/apex/com.android.art
PROBE: root.entries=[apex, data, dev, linkerconfig, opt, proc, sys, system, vendor]
PROBE: data.files=[rooms.json]
PROBE: rooms.json.head=[{"name":"uvroot-shadow-room", ...
PROBE-OK   shadow.write (32 bytes)
PROBE: calcPanelGrid.full=48                       --- 4.8×3.6 m 房间整板 48 块
PROBE: calcRoom.areaM2=17.28                       --- 应用自己的面积计算
PROBE: calcRoom.mainKeelQty=8
PROBE: calcRoom.crossKeelQty=11
PROBE: calcRoom.hangerQty=20
PROBE: calcRoom.trimQty=6
PROBE: calcAll.totalArea=17.28                     --- UI 用的整条汇总链路
PROBE: calcAll.panelsOpt=48
PROBE: calcAll.mainMeters=19.2
PROBE: LayoutVerts.build.floats=112                --- 预览用的几何顶点数组
PROBE-OK   dlopen:libc++_shared.so
PROBE-OK   dlopen:libceilingvk.so                  --- 应用的 Vulkan 原生库
PROBE: summary.pass=19 summary.fail=0
```

即：**应用的 dex、计算引擎、几何构造和原生库，都在隔离根里真实执行了**，
而不是“猜它能跑”。

> `LayoutVerts.build()` 对 `Room.poly` 为 null 的房间会直接返回空数组，
> 所以探针给房间补了轮廓（`Poly.pts` = 4 个 `double[]{x,y}`）才拿到 112 个顶点。

---

## 5. 数据隔离：写只落在影子里

影子目录是脚本自己创建并以**非 root** 身份拥有的，`rooms.json` 内容是我们编的
（`uvroot-shadow-room`）。探针在 guest 内往 `files/` 写了一个 marker：

```
ok   the probe's write landed in the shadow
ok   shadow still holds only our file set        (rooms.json uvroot-probe-marker.txt)
deny the real data dir is unreadable             (uid 10117 连列目录都被拒)
```

真实数据目录 `drwx------ u0_a114`，调用者 uid 10117 在**内核层面**就读不到、
更写不了 —— 所以“没污染真实数据”不是靠约定，是 DAC 保证的。

---

## 6. 读写隔离与假身份

| 用例 | 结果 |
|---|---|
| `--read-only` 下 guest 内写 `/data/data/<pkg>/files/never` | 拒绝，且宿主侧确实没生成该文件 |
| `--read-only` 下读影子 `rooms.json` | 正常 |
| `-i 0:0` 下 `id -u` | `0` |
| `-i 0:0` 下读影子（宿主属主是 10117） | 正常（uvroot 做 id 映射） |
| 上述操作后宿主侧 `stat -c %u` | 仍是 `10117`，元数据未改 |

---

## 7. 回测：用一个“最想突破隔离”的应用来验证隔离到底有没有效果

问题：上面这些结论是不是只是 uvroot 自己说自己隔离了？换一个**职业就是到处翻文件**的
应用来验：**MT 管理器**（`bin.mt.plus` 2.26.7，uid 10107，5 个 dex / 34 MB，
11 个原生库，含反调试的 `libmtprotect.so`）。

做法是 A/B：同一份探针、同一个 uid 10117、同一份 MT 原生库，
**一次不套 uvroot，一次套 uvroot**，然后对差。

测量工具尽量用 **MT 自己的原生代码**（`bin.mt.plus.Features` / `Features3`，
libmt1/libmt3 导出的 JNI）：

```
-- MT Manager's *own* readlink(), baseline vs isolated --
     /sdcard     : /storage/self/primary   ->   null
     /mnt/sdcard : /storage/self/primary   ->   null
     /etc        : /system/etc             ->   null
```

即：同一个 `readlink` 调用，隔离外能解析到真实路径，隔离内直接归零。`exists()` 侧同样：

| 路径 | 不套 uvroot | 套 uvroot |
|---|---|---|
| `/sdcard` `/storage` `/storage/emulated/0` `/mnt/sdcard` `/mnt` `/etc` `/data/media` `/data/data/com.termux` | `true` | **`false`** |
| `/system/bin/app_process64` `/vendor` `/linkerconfig` | `true` | `true`（被绑定） |
| `/data/app/bin.mt.plus/base.apk` | `false` | **`true`**（uvroot 凭空造出来的映射） |

MT 自己的库和 dex 在隔离内也照常工作，说明不是“因为跑不起来所以看不见”：

```
PROBE: Features.getABI=arm64-v8a
PROBE: Features.uid2name(10107)=u0_a107
PROBE-OK   dlopen:libmt1.so / libmt3.so / libmt2.so / libmtprotect.so / libterm.so
PROBE-OK   dex: bin.mt.plus.Features / bin.mt.plus.Features3
```

**关键一点**：`/sdcard` 在隔离内不是“权限不够”，而是**路径根本不在命名空间里**。
所以哪怕进程拥有 MT 管理器真正的 all-files 访问权（uid 10107），也一样什么都看不到——
uvroot 这一层的隔离比单纯 DAC 更硬。

复现：`DEVICE=<serial> ./run-as-termux.sh cases/66-mt-isolation.sh`（**39/39 全绿**，全程非 root）。

两个观察到的边界（不影响上面结论，如实记录）：

- 探针 `Class.forName("bin.mt.plus.Main")` 会抛
  `UnsatisfiedLinkError: No implementation found for void l.ۡ᩹ۨ.ۡ᩸ۛ(int)`——
  加载 `bin.mt.plus.Main` 会触发某个混淆类的静态初始化，而它要的 native 方法是
  MT 在真实 application 启动阶段才注册的，脱离真实应用进程就补不上
  （和 §8 GUI 边界是同一件事）。
- `Features3.startMTIO(path, mode)` 返回 `-1`，并在 stderr 打印 `Argument error.`：
  MTIO 需要应用启动时的初始化（`Features2.init(...)`），我们没有复刻。
  因此本轮用的是它的 `readlink`/`getABI`/`uid2name`，而不是 MTIO。

---

## 8. GUI 边界：为什么“完整隔离地跑界面”做不到

这一节是本次的重点，上一轮的结论在这里被**修正并补上系统侧证据**。

### 7.1 uvroot 起的进程不是“应用进程”

```
PROBE: android.uid=10117
PROBE: ActivityThread.currentActivityThread=null
PROBE: ActivityThread.currentApplication=null
PROBE: WindowManagerService=android.view.IWindowManager$Stub$Proxy@...
PROBE: WindowSession=FAILED: java.lang.NullPointerException: ... Looper.mQueue ... null
```

uvroot 起的是**普通进程**：没有 `ActivityThread`、没有 `Application`、没有
AMS 分配的窗口 token。它能加载应用的类、跑应用的计算，但它在外人眼里从来不是
“com.matecal.ceiling 这个应用”，因此**无法拥有窗口**。

### 7.2 从隔离根里发起启动：`am` 不行

```
$ run.sh /system/bin/am start -n com.matecal.ceiling/.MainActivity
Starting: Intent { cmp=com.matecal.ceiling/.MainActivity }
Exception occurred while executing 'start':
java.lang.SecurityException: Permission Denial:
  package=com.android.shell does not belong to uid=10117
    at ActivityTaskManagerService.assertPackageMatchesCallingUid(...)
```

`/system/bin/am` 只是 `cmd activity` 的包装，`ActivityManagerShellCommand` 把
caller 包名**写死成 `com.android.shell`**，uid 10117 不属于它 → 直接拒绝。

### 7.3 自己 binder 调：调用能过，策略拦下（**上一轮结论有误**）

用 `IActivityTaskManager.startActivityAsUser(..., callingPackage="com.termux", ...)`
可以绕过上面那条包名检查，返回 **0**；但 AMS 日志显示它随后被后台启动策略拦掉：

```
I ActivityTaskManager: START u0 {act=... cmp=com.matecal.ceiling/.MainActivity} from uid 10117
W ActivityTaskManager: Background activity start [callingPackage: com.termux; callingUid: 10117;
    isCallingUidForeground: false; callingUidHasAnyVisibleWindow: false;
    callingUidProcState: CACHED_EMPTY; ... allowBackgroundActivityStart: false; ...]
```

上一轮记录的“✅ 应用进前台”是**当时 caller 恰好在前台**的偶然结果；调用者一旦
处于后台（`CACHED_EMPTY`、无可见窗口），AMS 一律按后台启动拒绝。

### 7.4 就算 UI 起来了，它也不在隔离根里

用 shell 身份正常启动应用（AMS 真的把界面拉起来了）：

```
topResumedActivity=ActivityRecord{... com.matecal.ceiling/.MainActivity ...}
app pid = 3744
-- /opt/harness（uvroot 的映射）出现在该进程里吗？  0
-- 它的根里有而 guest 里没有的路径:  sdcard=yes storage=yes etc=yes data/media=yes
-- 它实际读的 rooms.json:  [{"name":"开关测试", ...
   （guest 里读到的是 uvroot-shadow-room）
```

**应用进程是 zygote 在系统命名空间里 fork 的**，根本不会经过 uvroot；它的
`/proc/<pid>/root` 是真实根，读到的是真实数据。隔离只作用于“发起方”，不作用于
应用运行时 —— 这一条与上一轮一致，但现在有哈希/路径级证据。

（顺带一个事实：Android 自己已经给每个应用单独过滤了 `/data/data` 视图，
`ls /proc/<app_pid>/root/data/data` 只有它自己一个包。）

### 7.5 结论

- **无 root**：可以把应用的**代码 + 数据 + 原生库**放进一个 uvroot 隔离根里跑，
  这是一条完整可用、可复现的路（本文档 §4–§6）。
- **无 root 且要完整 GUI**：不可能。界面必须由 AMS 驱动，而 AMS 只会 fork 自己的
  zygote 子进程；把 `app_process` 塞进 uvroot 就同时失去了成为“应用进程”的资格。
  两者不可兼得，除非有 root 级命名空间注入或自定义 zygote —— 那已经不是 uvroot。

---

## 9. “非 root”到什么程度

| 路径 | 身份 | 用没用 `su` | 结果 |
|---|---|---|---|
| `cases/65-app-isolation.sh` | Termux uid 10117 | **用了一次**，仅为进入 Termux uid 并补上 `AID_INET` 等补充组（Android 的 `su <uid>` 会给出 gid=0、groups 缺失，见主 README §2.3） | 35/35 |
| `app-isolation/no-su-device.sh` | adb shell uid 2000 | **完全没用** | 20/20 |

也就是说：**uvroot 本身不需要任何特权**；`su` 只出现在“如何以 Termux 身份启动”
这一步，且已有零 `su` 对照做同样的事。

需要如实说明的一点：板子 SELinux 是 **Permissive**，而且经 `su -c a5run` 落地的
进程 SELinux 域名仍是 `u:r:magisk:s0`（不是 `untrusted_app`）。因为 Permissive，
这不改变强制结果；在 Enforcing 的机器上，域名会不同（比如
`u:r:untrusted_app:s0`），DAC（uid 10117）才是真正起作用的限制。

---

## 10. 文件清单

| 文件 | 作用 |
|---|---|
| `app-isolation/run.sh` | 隔离根启动器（POSIX sh，设备侧运行，非 root） |
| `app-isolation/AppProbe.java` | 在隔离根里跑 ART + 应用代码 + 原生库，输出 `PROBE:` 行 |
| `app-isolation/MtProbe.java` | 驱动 MT 管理器自己的 JNI（`getABI`/`uid2name`/`readlink`），输出可 A/B 对比的可见性表 |
| `app-isolation/WindowProbe.java` | 证明隔离进程没有 `ActivityThread`/`Application`/窗口会话 |
| `app-isolation/AmStart.java` | 从隔离根 binder 调 `startActivityAsUser` |
| `app-isolation/build-and-push.sh` | 宿主侧 javac + d8 → `harness.zip` → push |
| `app-isolation/no-su-device.sh` | 零 `su` 设备侧套件（uid 2000） |
| `app-isolation/no-su-check.sh` | 零 `su` 宿主侧入口 |
| `app-isolation/run-suite.sh` | 一键：构建 + 跑 65 号用例 + 抓 GUI 边界证据 |
| `cases/65-app-isolation.sh` | 设备侧主套件（吊顶计算器，Termux 身份，35 项） |
| `cases/66-mt-isolation.sh` | MT 管理器 A/B 回测（套 / 不套 uvroot 对差，39 项） |

---

## 11. 坑与边界（写这部分时踩到的）

1. **`${VAR:?word}` 里的英文撇号会炸 dash**：`${APP_CODE:?the app's dir}` 会让
   dash 把 `'` 当成开引号一路吞到文件尾，报 `run.sh: 41: Syntax error:
   Unterminated quoted string`。写 `:?` 提示语时别用撇号。
2. **guest 的 `PATH` 要自己给**（见 §2 注）。
3. **`LayoutVerts.build()` 需要 `Room.poly`**，只给 `rects` 会静默返回空数组。
4. **`am`/`cmd` 的固定 caller 包名**是 `com.android.shell`，任何非 shell uid 都过不去。
5. **后台启动限制**与 caller 是否在前台强相关，不能把一次成功当成能力。
6. 真实应用数据由别的 uid 拥有且 0700，**非 root 复制不出来**；做影子数据要自己造，
   或者由 root 复制一次（上一轮的做法）。本套件选择自己造，因此全程不需要 root。
7. **A/B 对照里，baseline 必须在 `env -u LD_LIBRARY_PATH` 下跑**：Termux 的
   `LD_LIBRARY_PATH=$PREFIX/lib` 会遮住 `/system/lib64`，直接跑系统
   `app_process64` 会报
   `CANNOT LINK EXECUTABLE "/system/bin/app_process64": cannot locate symbol
   "Xzs_Construct" referenced by "/system/lib64/libunwindstack.so"`。
   隔离内反而没这个问题——`$PREFIX/lib` 根本没被绑定，动态加载器找不到就忽略。
8. **`getv()` 这类从 `PROBE:` 行取值的小工具别用 `sed` 拼路径**：路径里的 `/`
   会被当成 `s///` 的分隔符（`sed: unknown option to 's'`）。
   用 `grep -F -m1 "key=" | cut -d= -f2-` 更稳。
