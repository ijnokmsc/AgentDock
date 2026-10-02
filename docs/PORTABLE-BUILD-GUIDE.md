# Hermes Portable 便携版：实现原理与自建构建指南

> 目标：不再依赖别人打包好的便携版，仅凭两个上游仓库
> `EKKOLearnAI/hermes-studio`（web-ui）与 `NousResearch/hermes-agent`（Agent），
> 从零构建一个自包含、可移动、跨机器的 Hermes Portable。

---

## 1. 一句话原理

便携版 = **三个可移植运行时**（Python + Node + uv）+ **Agent 源码**（editable 安装）
+ **web-ui 前端**（npm 全局包）+ **一套 HOME 重定向启动脚本**（Hermes.bat）。

其中**可移动性**（拷到 U 盘/任意路径都能跑）靠三类关键技巧实现：
`pyvenv.cfg` 可重定位、`editable finder` 相对路径化、`HOME`/`USERPROFILE` 劫持。

---

## 2. 目录结构与职责

```
HermesPortable/
├── Hermes.bat / .sh / .command   # 启动器（按平台挑一个）
├── uv.exe                        # Python 包管理器（uv）
├── python/                       # 可重定位 Python 3.12（python-build-standalone）
│   └── cpython-3.12-windows-x86_64-none/python.exe
├── hermes-agent/                 # Agent 源码（git clone 自 NousResearch/hermes-agent）
│   ├── hermes_cli/  gateway/  agent/  ...   # 源码树
│   ├── tools/build.py            # ★ 便携版构建脚本（随源码带出）
│   └── pyproject.toml            # 版本号 0.21.x
├── venv/                         # uv 创建的虚拟环境（--relocatable）
│   ├── Scripts/hermes.exe        # hermes CLI 入口
│   └── Lib/site-packages/
│       ├── __editable___hermes_agent_*_finder.py  # editable 路径映射（已相对化）
│       └── hermes_agent-*.dist-info
├── node/                         # Node.js 运行时
│   ├── node.exe
│   └── node_modules/
│       └── hermes-web-ui/        # ★ web-ui（npm install -g hermes-web-ui@latest）
│           └── dist/server/index.js   # 服务端入口
├── data/                         # 运行时数据（session/skills/logs/config.yaml/.env）
├── _home/                        # 虚拟用户主目录（HOME 劫持目标）
│   └── .hermes -> ../data        # junction/symlink，指向 data
├── lib/                          # 启动器内部脚本（config_server.py / update.py / fix_*.py）
├── studio/                       # 启动器（HermesStudio.exe）私有数据
├── plugin/                       # 插件（plugin.dll + plugin.json）
├── tools/build.py                # 重复构建用的脚本副本
├── icons/ fonts/ tabler/         # 配置面板静态资源
└── runtime/desktop/              # （可选）官方桌面应用
```

---

## 3. 核心原理详解

### 3.1 两个上游组件如何接入

| 组件 | 来源 | 接入方式 | 产物 |
|---|---|---|---|
| **hermes-agent** | `NousResearch/hermes-agent`（源码） | `git clone` + `uv pip install -e` | 源码树 `hermes-agent/` + venv 里的 editable finder |
| **web-ui** | `EKKOLearnAI/hermes-studio` 发布的 **npm 包** `hermes-web-ui` / `ekko-studio` | `npm install -g hermes-web-ui@latest` | `node/node_modules/hermes-web-ui/` |
| Python | 官方 python-build-standalone | `uv python install 3.12` | `python/cpython-3.12-*/` |
| Node | nodejs.org 预编译包 | 下载解压 | `node/` |

> **注意**：web-ui **不是** clone hermes-studio 仓库，而是用它发布的 **npm 包**。
> 仓库 `EKKOLearnAI/hermes-studio` 通过 npm registry 发布 `hermes-web-ui`（遗留名）和
> `ekko-studio`（主包，同一版本），`npm install -g hermes-web-ui` 即可拿到。
> 构建脚本里硬编码了该 npm 包名，见 `tools/build.py` 的 `step_nodejs()`。

### 3.2 可移动性的三大支柱

#### (1) Python 可重定位 —— `pyvenv.cfg`
`uv venv --relocatable` 生成，`pyvenv.cfg` 里 `home` 指向 python 安装目录。
便携包移动后 `home` 是绝对路径会失效，**构建脚本/启动脚本需同步修正**（见 5 的坑）。

#### (2) editable 安装可移动 —— `__editable__*_finder.py` 相对化
`pip install -e` 会把**构建机的绝对路径**写进 finder 文件：
```python
MAPPING = {'hermes_cli': 'D:\\a\\...\\hermes-agent\\hermes_cli', ...}
```
换机器后该路径不存在 → `ModuleNotFoundError: No module named 'hermes_cli'`。

**修复**（`_fix_editable_paths` / `lib/fix_editable_paths.py`）：从 `__file__` 反向定位
源码树（`while _CUR: if exists(_CUR/hermes-agent): break; _CUR=parent`），把绝对路径
替换成相对解析：
```python
_CUR = dirname(abspath(__file__))
while _CUR and _CUR != dirname(_CUR):
    if isdir(join(_CUR, 'hermes-agent')): break
    _CUR = dirname(_CUR)
_HERMES_AGENT = join(_CUR, 'hermes-agent')
MAPPING = {'hermes_cli': _HERMES_AGENT + '/hermes_cli', ...}
```
因为相对 `__file__` 解析，整个文件夹**拖到任何盘符/路径都可用**，且幂等。

#### (3) HOME 劫持 —— `Hermes.bat` 的 sandbox
```bat
set "SANDBOX=%HERE%\_home"
set "HOME=%SANDBOX%"
set "USERPROFILE=%SANDBOX%"
set "HERMES_HOME=%HERE%\data"
```
所有读 `%USERPROFILE%\.hermes` 的工具（Python `os.path.expanduser`、Node/npm）都会
落到便携包内 `_home/`，**不污染宿主 C 盘**。`_home\.hermes` 是指向 `data/` 的
junction（NTFS 免管理员）或 symlink（FAT32 需开发者模式）。

### 3.3 web-ui 如何发现 hermes-agent（运行时对接）
web-ui 服务端启动时**主动探测** Agent（`Pp()`/`QXn()` 函数）：
1. 从 `HERMES_BIN` 环境变量（若设）或 PATH 里找 `hermes` CLI
2. 从 venv / python 可执行文件**逐级向上**，找含 `run_agent.py` 的目录作为 `agentRoot`
3. 候选路径包括 `.../hermes-agent`、`.../lib/hermes-agent` 等

便携包中 Hermes.bat 设置 `HERMES_BIN` 指向 `venv\Scripts\hermes.exe`，web-ui 据此拉起
Agent bridge。**所以 hermes-agent 源码树必须存在且含 `run_agent.py`**。

---

## 4. 自建便携版：构建方式

### 4.0 前置条件
- 一台可联网的机器（构建时需下载 uv / Python / Node / npm 包）
- Windows / macOS / Linux 均可（脚本跨平台）
- `git`、`curl`、系统 Python 3.10+（用于跑 build.py，或直接用便携版里的 python）

### 4.1 最简路径：复用官方 build.py

`build.py` 已随 hermes-agent 源码带出（`hermes-agent/tools/build.py`），
**它就是官方/第三方打包的同一份脚本**。拿到源码即可复现：

```bash
# 1. 拿到 hermes-agent 源码（含 tools/build.py）
git clone --depth 1 https://github.com/NousResearch/hermes-agent.git

# 2. 跑构建（默认带 desktop；--no-desktop 只要 CLI+web-ui）
cd hermes-agent
python3 tools/build.py --no-desktop --output D:/MyHermes

# 3. 产物在 D:/MyHermes/HermesPortable/
#    Windows 双击 Hermes.bat；macOS 双击 Hermes.command；Linux ./Hermes.sh
```

`build.py` 会自动完成（STEPS 顺序）：
| # | 步骤 | 做什么 | 网络依赖 |
|---|---|---|---|
| 1 | Downloading uv | 下载/复制 uv | github uv release |
| 2 | Installing portable Python | `uv python install 3.12` | uv/python-build-standalone |
| 3 | Cloning hermes-agent | `git clone hermes-agent` | **github** |
| 4 | Creating venv + deps | `uv venv --relocatable` + `uv pip install -e hermes-agent[extras]` | **PyPI** |
| 5 | Setting up data | 建 data/ + config.yaml + _home/.hermes | 无 |
| 6 | Downloading Node.js | 下载 node | nodejs.org |
| 7 | (内置) 装 web-ui | `npm install -g hermes-web-ui@latest` | **npm registry** |
| 8 | Copying launchers | 复制 Hermes.bat / lib/ / icons 等 | 无 |
| 9 | Writing README / Cleaning | 写说明 + 清缓存 | 无 |

### 4.2 从上游 releases 构建（不依赖第三方便携包）

如果不想用 build.py 里写死的默认 tag，可**指定 hermes-agent 的 release tag**：
```bash
# 方式一：改 build.py 里 HERMES_TAG
#   HERMES_TAG = "v0.21.3"   # 指定版本；None = 最新 main

# 方式二：手动构建（完全可控，见 4.3）
```
web-ui 侧，`npm install -g hermes-web-ui@0.7.23` 即可锁定版本（releases 发布到 npm）。

### 4.3 手动构建（完全可控，理解每一步）

若想彻底掌握，可跳过 build.py 逐步执行（等价于 build.py 的 STEPS）：

```bash
# 在目标目录 HermesPortable/
ROOT=HermesPortable
mkdir -p $ROOT && cd $ROOT

# (1) uv —— 下载或复制
curl -fsSL -o uv_tmp.zip https://github.com/astral-sh/uv/releases/latest/download/uv-x86_64-pc-windows-msvc.zip
unzip -o uv_tmp.zip -d . && mv uv_tmp/uv.exe ./uv.exe && rm -rf uv_tmp uv_tmp.zip

# (2) 可重定位 Python 3.12
UV_PYTHON_INSTALL_DIR=$PWD/python ./uv.exe python install 3.12 --install-dir $PWD/python

# (3) clone hermes-agent
git clone --depth 1 https://github.com/NousResearch/hermes-agent.git

# (4) venv + editable 安装
./uv.exe venv venv --python $PWD/python/cpython-3.12-*/python.exe --relocatable
./uv.exe pip install -e "hermes-agent[cron,messaging,cli,mcp,web,tts-premium]" --python venv/Scripts/python.exe

# (5) 相对化 editable finder（关键！不然后面 import hermes_cli 失败）
python - <<'PY'
import pathlib
sp = pathlib.Path("venv/Lib/site-packages")
for f in sp.glob("__editable__*_finder.py"):
    t = f.read_text(encoding="utf-8")
    inject = (
        "import os as _os\n"
        "_CUR = _os.path.dirname(_os.path.abspath(__file__))\n"
        "while _CUR and _CUR != _os.path.dirname(_CUR):\n"
        "    if _os.path.isdir(_os.path.join(_CUR, 'hermes-agent')):\n"
        "        break\n"
        "    _CUR = _os.path.dirname(_CUR)\n"
        "_HERMES_AGENT = _os.path.join(_CUR, 'hermes-agent')\n\n"
    )
    t = t.replace("MAPPING:", inject + "MAPPING:", 1)
    t = t.replace("'" + str(pathlib.Path("hermes-agent").resolve()).replace("\\","/") + "/", "_HERMES_AGENT + '/")
    f.write_text(t, encoding="utf-8")
PY

# (6) 下载 Node + 装 web-ui
curl -fsSL -o node.zip https://nodejs.org/dist/v22.15.0/node-v22.15.0-win-x64.zip
unzip -o node.zip && mv node-v22.15.0-win-x64/* node/ && rmdir node-v22.15.0-win-x64 node.zip
PATH="$PWD/node:$PATH" npm install -g hermes-web-ui@latest --force

# (7) 建 data/ + _home/.hermes 链接
mkdir -p data/{sessions,skills,logs,memories,cron,plugins} _home
# _home/.hermes -> ../data（Windows 用 mklink /J，需 NTFS；FAT32 需开发者模式）

# (8) 放启动脚本 Hermes.bat（见 4.4 或从 build.py 的 _STATIC_ASSETS 复制）
```

> Windows 上手工建 junction：`cmd /c mklink /J "HermesPortable\_home\.hermes" "HermesPortable\data"`

### 4.4 启动脚本 Hermes.bat 要点（必需）

一份可用的 Hermes.bat 至少要包含（可参考便携包现有版本）：
1. `set "HERE=%~dp0"`（去尾斜杠）—— 所有路径锚点
2. 检测 venv / python / node 位置（多布局兼容）
3. **HOME 劫持**：`set HOME=%HERE%\_home`、`set USERPROFILE=%HERE%\_home`、`set HERMES_HOME=%HERE%\data`
4. 设置 `HERMES_BIN=%HERE%\venv\Scripts\hermes.exe`（web-ui 据此拉起 Agent）
5. `PYTHONHOME` / `PYTHONUTF8` / `PYTHONIOENCODING`（python-build-standalone 需要）
6. 每次启动跑 `lib/fix_shims.py` 和 `lib/fix_editable_paths.py`（保证可移动）
7. 单实例锁 + 启动 web-ui（`node.exe index.js start 8648`）

---

## 5. 常见坑与修复（构建/运行时）

### 5.1 `ModuleNotFoundError: No module named 'hermes_cli'`
**原因**：editable finder 里还是构建机的绝对路径。
**修复**：跑 `lib/fix_editable_paths.py`（或重跑 4.3 步骤 5 的相对化脚本）。

### 5.2 `No Python at 'D:\a\...python.exe'`
**原因**：`venv/pyvenv.cfg` 的 `home` 是构建机的绝对路径。
**修复**：把 `home` 改成当前 `python/` 的实际路径：
```
home = <当前路径>\python\cpython-3.12-windows-x86_64-none
```

### 5.3 web-ui 起不来 / `hermes-web-ui not bundled`
**原因**：`npm install -g` 失败（Windows CI 常见 DNS EAI_FAIL）。
**修复**：换 registry + 强制 IPv4：
```bash
npm install -g hermes-web-ui@latest --force --registry https://registry.npmmirror.com/ --dns-result-order=ipv4first
```

### 5.4 便携包移动后损坏
- `_home/.hermes` 符号链接断 → 重建 junction 指向新的 `data/`
- `pyvenv.cfg` 的 home 失效 → 见 5.2
- `settings.json` 里的 `hermesRoot` 是旧绝对路径 → 启动器 autoDetect 会自动纠正（相对路径化）

### 5.5 Windows 上 Node 版本选择
- Node 24 在 Windows CI 会崩 `ncrypto::CSPRNG`，**Windows 用 Node 22 LTS**（如 22.15.0）
- macOS/Linux 用 Node 24 LTS

---

## 6. 与启动器（HermesStudio.exe）的关系

`HermesStudio.exe`（本仓库）是便携包的**图形启动器**，它**不是**便携包的必要组成部分，
而是可选的管理层（启动/停止 Hermes、体检、更新、插件）。便携包本身用 `Hermes.bat`
即可运行。

若希望便携包带 GUI 启动器，需额外：
1. 编译 `HermesStudio.exe` + 5 个 Qt6 DLL + Qt 插件目录（platforms/styles/tls 等）放到便携包根
2. 启动器 `autoDetect()` 定位 `Hermes\` 子目录（或根目录直接是内容）
3. 启动器负责：渲染 `.env`/`config.yaml` → 调 `Hermes.bat`/`Launcher` 拉起 web-ui

---

## 7. 更新与版本管理

- **hermes-agent**：`git pull`（若保留 .git）或重新 `uv pip install -e`；版本号在 `pyproject.toml`
- **web-ui**：`npm install -g hermes-web-ui@<新版>`；版本号在 `node/node_modules/hermes-web-ui/package.json`
- 便携包自带的 `lib/update.py` 封装了上述更新（参考其实现）
- 更新后务必重跑 `fix_editable_paths.py`（若 agent 源码变过）并验证 `import hermes_cli`

---

## 8. 直接使用官方预编译运行时（推荐最快路径）

> 用户发现：`EKKOLearnAI/hermes-studio` 的 releases 里有 **`hermes-0.21.3-runtime`**，
> 官方已把 Hermes 核心运行时编译好并发布，**无需自己构建 Python/Node/hermes-agent**。

### 8.1 这个 runtime release 提供什么

`hermes-0.21.3-runtime`（2026-09-16）提供各平台**完整预编译运行时**（每个 300~570MB）：

| 平台 | 资产 | 大小 |
|---|---|---|
| **Windows x64** | `hermes-runtime-hermes-agent-0.21.3-win-x64.tar.gz` | 543MB |
| macOS x64 / arm64 | `...-mac-x64.tar.gz` / `...-mac-arm64.tar.gz` | ~493MB |
| Linux x64 / arm64 | `...-linux-x64.tar.gz` / `...-linux-arm64.tar.gz` | 300~530MB |
| 各平台 | `hermes-runtime-<platform>.json`（清单） | 小 |

### 8.2 runtime 内部结构（官方 `package-runtime.mjs` 打包）

```
runtime 解包后/
├── python/                 # 完整 Python + venv + hermes-agent 源码
│   ├── base/               #   Python 运行时（pyvenv.cfg home=../base，相对可重定位）
│   ├── venv/               #   uv 虚拟环境（editable 安装 hermes-agent）
│   │   └── Scripts/hermes.exe
│   ├── hermes-agent/       #   hermes-agent 源码（含 run_agent.py、.git）
│   └── run_agent.py
├── node/                   # Node.js 运行时（不含 web-ui！）
├── git/                    # Git for Windows（仅 Windows）
└── runtime-manifest.json   # 版本/来源清单（schema 2）
```

**要点**：
- runtime 里**不含 web-ui**——web-ui 是 Ekko Studio 桌面端自带/单独 npm 包
- 官方打包时已验证**可重定位**（移动 staging 后 `import hermes_cli` 仍成功）
- pyvenv.cfg 的 `home` 已写成相对路径 `../base`（`makeBundledBaseConfigPortable`）
- 来源锁定：`NousResearch/hermes-agent.git` @ `v2026.9.14`（commit 345cd2b）

### 8.3 用 runtime 组装便携版（win-x64 示例）

```bash
# 1. 下载官方预编译运行时
curl -LO https://github.com/EKKOLearnAI/hermes-studio/releases/download/hermes-0.21.3-runtime/hermes-runtime-hermes-agent-0.21.3-win-x64.tar.gz

# 2. 解包为便携包骨架
mkdir -p HermesPortable && cd HermesPortable
tar -xzf ../hermes-runtime-hermes-agent-0.21.3-win-x64.tar.gz
# 解出 python/ node/ git/ runtime-manifest.json

# 3. 校验 sha256（官方清单）
#    hermes-runtime-win-x64.json 里的 asset.sha256

# 4. 装 web-ui（runtime 不含，需单独装）
PATH="$PWD/node:$PATH" npm install -g hermes-web-ui@0.7.23 --force
#   或从 Ekko Studio 桌面版提取 node/node_modules/hermes-web-ui
#   web-ui 装到 node/node_modules/hermes-web-ui（或 node/lib/node_modules）

# 5. 建 data/ + _home/.hermes + 启动脚本
mkdir -p data/{sessions,skills,logs,memories,cron,plugins} _home
# _home/.hermes -> ../data（Windows: cmd /c mklink /J ...）
# 放 Hermes.bat（见 4.4）/ lib/（见下）

# 6. 启动
./Hermes.bat        # 或直接 node node_modules/hermes-web-ui/dist/server/index.js start 8648
```

### 8.4 对比：源码构建 vs releases 预编译

| 维度 | 源码构建（第 4 章） | releases 预编译（本章） |
|---|---|---|
| 耗时 | 数十分钟（下载+编译+装依赖） | 数分钟（只下载+解包） |
| 网络 | github + PyPI + npm 全量 | 一个 tar.gz + npm |
| 版本锁定 | 自选 hermes-agent tag | 官方锁定的 v2026.9.14 |
| web-ui | 自选 npm 版本 | 同左（仍需单独装） |
| 可控性 | 高 | 中（跟随官方 runtime） |
| 可移动性 | 需跑 fix_editable_paths | 官方已做相对化，需二次验证 |

### 8.5 注意

- 官方 runtime 是为 **Ekko Studio 桌面端**设计的 managed-runtime，但内容（python+node+hermes-agent）恰好是便携版所需；web-ui 需自行补装。
- 若想完全脱离第三方，可用**源码构建**（第 4 章）；若想最快拿到可用便携版，用**本章 releases 预编译**。
- 官方 runtime 的 `python/base/` 布局与便携包 `python/cpython-*/` 不同，组装时注意 `pyvenv.cfg` 的 `home` 指向。

