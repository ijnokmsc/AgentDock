# 便携版泄漏审计：跑完是否在包外留文件

> 审计对象：`D:\text\HermesPortable`（hermes-agent 0.21.3 + web-ui 0.7.23）
> 审计时间：2026-09-19
> 工具：`scripts/leakcheck/snapshot.ps1`（只读，不删除任何文件）
> 结论：**修复前有 2 条泄漏通道，修复后包外零新增。**

---

## 1. 结论速览

| 通道 | 落点（宿主） | 修复前 | 修复后 |
|---|---|---|---|
| npm 缓存 / 调试日志 | `%LOCALAPPDATA%\npm-cache\_logs\*.log` | ❌ 每次启动 6 个文件 | ✅ 0 |
| Node 编译缓存 | `%TEMP%\node-compile-cache\<ver>-x64-*\` | ❌ 冷缓存时数千条目 | ✅ 0 |
| Hermes 自身状态 | `~/.hermes`、`~/.hermes-web-ui` | ✅ 已被 HOME 劫持挡住 | ✅ 0 |
| Python/uv/pip 缓存 | `~/.cache`、`~/.uv` | ✅ 已被 HOME 劫持挡住 | ✅ 0 |
| Windows 临时文件 | `%TEMP%\*` | ⚠️ 未专门验证 | ✅ 0 |

一句话：**只劫持 `HOME`/`USERPROFILE` 是不够的**。Windows 原生消费者（尤其 npm）根本不看 `HOME`。

---

## 2. 为什么"便携"很容易漏

三类程序取"用户目录"的方式完全不同，便携化必须三条都堵：

| 取法 | 例子 | 只看哪个变量 |
|---|---|---|
| POSIX 风格 | Python `os.path.expanduser`、`os.homedir()`、git | `HOME` / `USERPROFILE` |
| Windows 已知文件夹 | 多数 .NET / Qt 程序 | `APPDATA` / `LOCALAPPDATA` |
| **硬编码** | **npm**（Windows 版把 cache 写死成 `%LOCALAPPDATA%\npm-cache`，global prefix 写死成 `%APPDATA%\npm`） | 直接读 `LOCALAPPDATA` / `APPDATA` |
| 进程级临时目录 | Node 编译缓存、`tempfile` | `TEMP` / `TMP` |

原来 `Hermes.bat` 只做了第 1 类 → 第 2/3/4 类全部漏到宿主。

---

## 3. 审计方法（可复现）

不靠"看目录存不存在"，靠**时间水位线 + 前后差集**。宿主上本来就有用户日常正装的
`~/.hermes-web-ui`（含 `.ekko` 库），用存在性判断会把别人的数据算到自己头上。

```
scripts/leakcheck/snapshot.ps1 -Watermark <ISO时间> -Out <报告>
```

脚本输出三段（全部只读）：

- `[A]` 宿主上所有 hermes 相关目录的**完整文件名清单** → 用于前后集合差
- `[B]` 宿主扫描范围内 **mtime ≥ 水位线** 的每个文件 → 用于时间差
- `[C]` 便携包内 mtime ≥ 水位线 的文件 → 反证"该写的都写在包内"

扫描范围：`C:\Users\<me>`（递归，含 AppData/LocalLow）+ `C:\Windows\Temp`。

**操作序列**：记水位线 → 采前快照 → 启动便携版 → 等端口就绪并触达 HTTP →
跑满 ~8 分钟 → 停进程 → 采后快照 → 求差。

---

## 4. 第一轮：修复前发现

水位线 `2026-09-19T21:12:30`，运行窗口 `21:12:35 → 21:21:00`（web-ui :8648 + agent-bridge :18765 均就绪）。

- `[B]` 宿主 mtime ≥ 水位线的文件：**365 个**
  —— 但绝大多数是系统噪音（Microsoft / 通义千问 / 抖音 / QQ / Discord / qBittorrent …）
  以及本次审计自己产生的文件。
- **其中与本包相关的：6 个**，全部落在同一目录、同一秒：

```
C:\Users\zhaoy\AppData\Local\npm-cache\_logs\2026-09-19T13_12_44_{418,453,514,569,615,659}Z-debug-0.log
```

- `[A]` 集合差：宿主 hermes 相关目录**新增 6 个文件**，就是上面这 6 个，其余为 0。
- `[B]` 里没有任何 `~/.hermes*`、`~/.cache`、`~/.npm`、`~/.uv`、`~/.config`、`~/.local`、
  `AppData\Local\hermes`、`AppData\Roaming\hermes` 下的新文件。
- `[B]` 里 `%TEMP%` 的 7 条全部可归因于 WorkBuddy / 系统自身，**无本包写入**。

### 根因（npm 自己的日志把它供出来了）

```
0 verbose cli D:\text\HermesPortable\node\node.exe ...\npm-cli.js
4 silly config load:file:D:\text\HermesPortable\_home\.npmrc      ← HOME 劫持生效
5 silly config load:file:D:\text\HermesPortable\node\etc\npmrc
7 verbose argv "prefix" "--global"
8 verbose logfile logs-max:10 dir:C:\Users\zhaoy\AppData\Local\npm-cache\_logs\  ← 漏了
```

web-ui 启动时会执行 `npm prefix --global`（一次性 6 次，250ms 内），
用来定位全局安装根。npm 在 Windows 上**无视 `HOME`**，把日志目录钉在
`%LOCALAPPDATA%\npm-cache\_logs`。每次启动 6 个文件，换台机器污染一台。

### 顺带挖出的第二条通道（第一轮没直接抓到，属"冷缓存才发作"）

宿主 `%TEMP%\node-compile-cache\v22.22.0-x64-9de703df\` 有 **2563 个条目**，按小时分布：

```
493  09-18 01
466  09-19 08
462  09-19 05
388  09-17 17
387  09-19 20   ← 上一次便携版运行
291  09-17 16
 76  09-19 19   ← 更早的便携版运行
```

19:xx / 20:xx 这两批正是此前两次启动便携版的时间。第一轮（21:12）之所以没新增，
是因为缓存已暖、node 无源码变更就不再改写 —— **典型的"跑一次不漏、跑一百次里漏一次"**。
Node 把编译缓存写在 `%TEMP%` 下，`TEMP` 当时未被劫持。

---

## 5. 修复

同时改两处，保持"手工启动"和"启动器启动"行为一致。

### 5.1 `D:\text\HermesPortable\Hermes.bat`

新增「沙箱目录预建 + 全量环境劫持」两段（仍在 `setlocal` 内，绝不改宿主）：

```bat
set "SANDBOX=%HERE%\_home"
set "HOME=%SANDBOX%"
set "USERPROFILE=%SANDBOX%"
set "APPDATA=%SB_ROAMING%"        rem _home\AppData\Roaming
set "LOCALAPPDATA=%SB_LOCAL%"     rem _home\AppData\Local
set "TEMP=%SB_TMP%"               rem _home\tmp
set "TMP=%SB_TMP%"
set "NPM_CONFIG_CACHE=%SB_NPMCACHE%"
set "NPM_CONFIG_LOGS_DIR=%SB_NPMCACHE%\_logs"
set "NPM_CONFIG_PREFIX=%SANDBOX%\npm-global"
set "NPM_CONFIG_USERCONFIG=%SANDBOX%\.npmrc"
set "UV_CACHE_DIR=..."  "PIP_CACHE_DIR=..."  "PYTHONPYCACHEPREFIX=..."
set "XDG_CACHE_HOME=..."  "XDG_CONFIG_HOME=..."  "XDG_DATA_HOME=..."
set "PLAYWRIGHT_BROWSERS_PATH=%SB_CACHE%\ms-playwright"
```

顺序很关键：**先把 `_home\...` 全部 `mkdir` 出来，再指过去**。
有些 runtime 在 `%TEMP%` 不存在时会直接失败。

### 5.2 启动器侧 `src/core/Launcher.cpp`

`hermesEnv()` 原来只注入 6 个 `HERMES_*`，现在与 `Hermes.bat` 等价：
先 `create_directories` 建齐沙箱目录，再返回同一组环境变量。
`ProcessUtil::start` 是"取当前进程环境 + 覆盖同名项"，所以新增项会正确覆盖。

配套改动：
- `Paths::sandboxDir()` → `<root>/_home`（`src/app/Paths.h` / `.cpp`）

---

## 6. 第二轮：修复后验证

同一套水位线方法，水位线 `2026-09-19T21:37:00`，运行窗口 `21:37:11 → 21:42`。

### 6.1 宿主侧

- `[B]` 宿主 mtime ≥ 水位线的文件：**330 个** —— 但逐条核对后**全部**是其它软件的系统噪音
  （通义千问 70、Microsoft 60、抖音 49、O+Connect 12、豆包输入法 10、Discord 8、
  腾讯文件 7、WorkBuddy 自身日志 7 …）。
- `[B]` 中匹配 `hermes` / `ekko` / `nous` / `agent-bridge` 的条目：只有 3 条，
  全部是 **WorkBuddy 自己**的会话日志（`~/.workbuddy/logs/.../HermesStudio__*.log`、
  `~/.workbuddy/projects/i-HermesStudio/*.jsonl`）——是本次审计的产物，与本包无关。
- `[B]` 中 `npm-cache` 条目：**0**（第一轮是 6）。
- `[B]` 中 `node-compile-cache` 条目：**0**。
- `[A]` 集合差（对照 `before.txt`）：新增条目 **6 个，且全是第一轮 21:12:44 那 6 个
  npm 日志**；第二轮（21:37+）新增 **0**。
- 计数器：`%LOCALAPPDATA%\npm-cache\_logs\` 文件数 17 → **仍是 17**；
  `~/.hermes-web-ui` 顶层条目 16 → **仍是 16**。

### 6.2 包内（反证：该写的都写进来了）

```
_home\.npm-cache\_logs\2026-09-19T13_37_19_{388..637}Z-debug-0.log   ← 6 个，npm 日志
_home\tmp\node-compile-cache\                                        ← Node 编译缓存
_home\.cache  _home\.config  _home\.local  _home\.uv  _home\AppData  ← 沙箱骨架
```

包内那份 npm 日志显示所有路径都已归位：

```
4 silly config load:file:D:\text\HermesPortable\_home\.npmrc
5 silly config load:file:D:\text\HermesPortable\_home\npm-global\etc\npmrc
8 verbose logfile ... dir:D:\text\HermesPortable\_home\.npm-cache\_logs\
```

### 6.3 包内状态文件（一直在包内，非泄漏）

- `data\` —— gateway / cron / logs / cache / skills（`HERMES_HOME` 指向）
- `webui\.ekko\` —— web-ui 的 ekko 库（`ekko.db`）
- `packages\server\data\hermes-web-ui.db` —— web-ui 服务端自己的库
- `python\venv\pyvenv.cfg` —— 每次启动被重写为绝对路径（可重定位的代价）

> 注意 `webui\.ekko` 和 `packages\server\data` 是 web-ui 按自身包布局推导出来的，
> 都在包内，但它们**不在 `data\` 下**。将来若提供"重置数据"，需要一并清理这四处，
> 否则会出现"重置了但历史记录还在"。

---

## 7. 尚未覆盖的边界（明确留白）

以下没有测，不宣称已封闭：

1. **`%ProgramData%`** —— 未扫描。当前两条链路都不写，但第三方 skill / MCP server 有可能。
2. **注册表 `HKCU`** —— 环境变量劫持对注册表无效。若某组件写 `HKCU\Software\...`，
   便携版无法阻止（也无法随包搬走）。
3. **`SHGetKnownFolderPath` 系 API** —— 直接问系统要真实路径，**无视环境变量**。
   本次实测未见任何组件使用；但这是环境变量劫持方案的原理性上限。
4. **`C:\Windows\Temp` 之外的系统目录**、非 C: 盘符 —— 未扫描。
5. **网络侧**（LAN 发现 UDP 48640/48648、遥测）—— 本地文件之外的话题，另议。
6. junction `_home\.hermes -> data` 在 FAT32 / exFAT U 盘上不支持，
   换到这类介质需要降级为普通目录 + 启动时同步。

---

## 8. 给 `PortableBuilder` 的落地清单

ADR-004 的 `PortableBuilder` 目前仍是 Proposed。实现时把上面这些**写进生成器**，
不要留在手工 `Hermes.bat` 里：

- [ ] 生成 `Hermes.bat` 时写入第 5.1 节那一整组环境变量（模板与 `hermesEnv()` 同源）
- [ ] 首次构建时预建 `_home\{AppData\Local,AppData\Roaming,tmp,.npm-cache,.cache,.config,.local\share,.uv\cache,npm-global}`
- [ ] 构建后自检：跑一次启动 + 停在就绪状态，比对包外快照（复用 `scripts/leakcheck/`）
- [ ] `Hermes.bat` 必须**纯 ASCII + CRLF**。cmd.exe 的批处理解析器不吃 UTF-8 中文，
      连 `rem` 注释里的中文都会把行切碎成乱码命令（`scripts/build.bat` 就踩了这个坑，
      见 §9）
- [ ] 生命周期同步 `Launcher::hermesEnv()` 与 bat 模板，避免两边漂移

---

## 9. 附：顺带记下的构建坑

`scripts\build.bat` / `scripts\env_msvc.bat` 是 **UTF-8 含中文**，cmd.exe 的批处理
解析器按 OEM 代码页（中文系统 = GBK）读字节，会把行切碎：

```
'cvarsall' is not recognized as an internal or external command
'的' is not recognized as an internal or external command
'S' is not recognized as an internal or external command
```

`chcp 65001` 也救不了（批处理解析不用控制台代码页）。可用替代：

```powershell
# PowerShell 里直接注入 MSVC 环境，绕开 bat
$VCROOT='E:\炫彩IDE\data\VC\VC2022\14.41.34120'
$WKROOT='E:\炫彩IDE\data\VC\Windows Kits\10'; $WKVER='10.0.19041.0'
$env:PATH="$VCROOT\bin\Hostx64\x64;$WKROOT\bin\$WKVER\x64;$env:PATH"
$env:INCLUDE="$VCROOT\include;$VCROOT\atlmfc\include;$WKROOT\Include\$WKVER\ucrt;$WKROOT\Include\$WKVER\shared;$WKROOT\Include\$WKVER\um;$WKROOT\Include\$WKVER\winrt"
$env:LIB="$VCROOT\lib\x64;$VCROOT\atlmfc\lib\x64;$WKROOT\Lib\$WKVER\ucrt\x64;$WKROOT\Lib\$WKVER\um\x64"
& 'I:\HermesStudio\_tmp\venv\Scripts\cmake.exe' --build 'I:\HermesStudio\build' --parallel
```

根治办法是把这两个脚本转成 GBK(ANSI) 编码，或改成纯 ASCII。

---

## 10. 复现命令

```powershell
# 1) 前快照（只采清单，不扫时间）
& scripts\leakcheck\snapshot.ps1 -Watermark 2099-01-01T00:00:00 `
    -Out scripts\leakcheck\out\before.txt -SkipNewFileScan

# 2) 记水位线 → 启动便携版 → 等 8648/18765 就绪 → 跑满 5~10 分钟 → 停进程
#    水位线取"启动前 5 秒"

# 3) 后快照
& scripts\leakcheck\snapshot.ps1 -Watermark 2026-09-19T21:37:00 `
    -Out scripts\leakcheck\out\after2.txt

# 4) 差集
Select-String '^B\|' scripts\leakcheck\out\after2.txt | ...
```

> 注意：`snapshot.ps1` 的 `[B]` 段要递归扫 140 万+ 条目，单次约 15 分钟。
> 扫描期间**不要开第二个 PowerShell 会话**（同一宿主会把它打断）。
