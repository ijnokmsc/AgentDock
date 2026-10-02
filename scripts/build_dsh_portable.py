#!/usr/bin/env python3
"""Build a DeepSeek Harness (dsh) portable package.

Produces a self-contained, relocatable package:
    <target>/
    +-- Dsh.bat          launcher script (ASCII + CRLF), pins all env into the package
    +-- node/            Node.js runtime (win-x64 zip from nodejs.org, sha256 verified)
    +-- dsh/             npm prefix holding node_modules/@deepseek-ai/dsh (pinned version)
    +-- data/dsh/        DSH_HOME (profiles/, sessions, user .env)
    +-- _home/           HOME/USERPROFILE/APPDATA/LOCALAPPDATA/TEMP pin target
    +-- studio/          launcher private data (cache/downloads, npm cache, pnpm)
    +-- runtime-manifest.json

Portability facts (verified 2026-09-23, see docs/ADR-008):
- dsh resolves all user data under $DSH_HOME (official env hook, dsh-home-paths).
- dsh's Windows module-fallback symlinks are junctions, re-pointed at every boot.
- Built-in profile bundles resolve from the installation dir: first boot is offline.

Usage:
    python scripts/build_dsh_portable.py --target I:/PortableAgent/DSH
Bootstrap node (only used to drive npm during the build):
    --bootstrap-node I:/HermesPortable/node/node.exe
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path
from datetime import datetime, timezone

NODE_DIST = "https://nodejs.org/dist"
DEFAULT_NODE_VERSION = "v24.21.0"
DEFAULT_DSH_VERSION = "0.1.5-rc.2"

DSH_BAT_TEMPLATE = r"""@echo off
setlocal EnableExtensions
set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"

rem ===== portable environment pinning: nothing outside this folder =====
set "DSH_HOME=%HERE%\data\dsh"
set "HOME=%HERE%\_home"
set "USERPROFILE=%HERE%\_home"
set "APPDATA=%HERE%\_home\AppData\Roaming"
set "LOCALAPPDATA=%HERE%\_home\AppData\Local"
set "TEMP=%HERE%\_home\Temp"
set "TMP=%HERE%\_home\Temp"
set "NPM_CONFIG_CACHE=%HERE%\studio\cache\npm-cache"
set "PNPM_HOME=%HERE%\studio\cache\pnpm"
set "PATH=%HERE%\node;%PATH%"

set "NODE_EXE=%HERE%\node\node.exe"
set "DSH_BIN=%HERE%\dsh\node_modules\@deepseek-ai\dsh\lib\bin.js"
if not exist "%NODE_EXE%" (
    echo [dsh] node.exe not found under %HERE%\node
    exit /b 1
)
if not exist "%DSH_BIN%" (
    echo [dsh] dsh not installed under %HERE%\dsh
    exit /b 1
)

"%NODE_EXE%" "%DSH_BIN%" %*
"""


def log(step: str, msg: str) -> None:
    print(f"[{step}] {msg}", flush=True)


def download(url: str, dest: Path) -> None:
    log("download", f"{url} -> {dest.name}")
    with urllib.request.urlopen(url, timeout=300) as resp, open(dest, "wb") as f:
        shutil.copyfileobj(resp, f)


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd: list, env: dict, cwd: Path = None, check: bool = True):
    proc = subprocess.run(cmd, env=env, cwd=str(cwd) if cwd else None,
                          capture_output=True, text=True, errors="replace")
    if check and proc.returncode != 0:
        sys.stderr.write(proc.stdout[-4000:] + "\n" + proc.stderr[-4000:] + "\n")
        raise SystemExit(f"command failed ({proc.returncode}): {' '.join(map(str, cmd))}")
    return proc


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True, help="portable package target dir")
    ap.add_argument("--node-version", default=DEFAULT_NODE_VERSION)
    ap.add_argument("--dsh-version", default=DEFAULT_DSH_VERSION)
    ap.add_argument("--bootstrap-node", default=r"I:\HermesPortable\node\node.exe",
                    help="existing node.exe used to run npm during the build")
    ap.add_argument("--registry", default=None, help="npm registry override (e.g. npmmirror)")
    args = ap.parse_args()

    target = Path(args.target).resolve()
    cache = target / "studio" / "cache" / "downloads"
    cache.mkdir(parents=True, exist_ok=True)
    log("prepare", f"target={target} node={args.node_version} dsh={args.dsh_version}")

    bootstrap_node = Path(args.bootstrap_node)
    if not bootstrap_node.is_file():
        raise SystemExit(f"bootstrap node not found: {bootstrap_node}")
    npm_cli = bootstrap_node.parent / "node_modules" / "npm" / "bin" / "npm-cli.js"
    if not npm_cli.is_file():
        raise SystemExit(f"npm-cli.js not found next to bootstrap node: {npm_cli}")

    # ---- 1. download + verify Node zip -----------------------------------
    node_zip = cache / f"node-{args.node_version}-win-x64.zip"
    shasums = cache / f"SHASUMS256-{args.node_version}.txt"
    base = f"{NODE_DIST}/{args.node_version}"
    if not node_zip.exists():
        download(f"{base}/node-{args.node_version}-win-x64.zip", node_zip)
    if not shasums.exists():
        download(f"{base}/SHASUMS256.txt", shasums)
    expected = next((l.split()[0] for l in shasums.read_text().splitlines()
                     if l.strip().endswith("node-" + args.node_version + "-win-x64.zip")), None)
    actual = sha256_of(node_zip)
    if not expected or actual != expected:
        node_zip.unlink(missing_ok=True)
        raise SystemExit(f"node zip sha256 mismatch: {actual} != {expected}")
    log("verify", f"node zip sha256 OK ({actual[:16]}...)")

    # ---- 2. extract Node -> node/ ----------------------------------------
    node_dir = target / "node"
    if node_dir.exists():
        log("extract", "removing old node/")
        shutil.rmtree(node_dir)
    node_dir.parent.mkdir(parents=True, exist_ok=True)
    log("extract", f"unzipping {node_zip.name}")
    with zipfile.ZipFile(node_zip) as z:
        top = {n.split("/")[0] for n in z.namelist()}
        if len(top) != 1:
            raise SystemExit(f"unexpected zip layout: {top}")
        z.extractall(target)
        shutil.move(str(target / top.pop()), str(node_dir))
    node_exe = node_dir / "node.exe"
    ver = run([str(node_exe), "--version"], env=os.environ).stdout.strip()
    if ver != args.node_version:
        raise SystemExit(f"extracted node reports {ver}, expected {args.node_version}")
    log("extract", f"node {ver} ready")

    # ---- 3. npm install pinned dsh -> dsh/ --------------------------------
    dsh_dir = target / "dsh"
    if dsh_dir.exists():
        log("dsh", "removing old dsh/")
        shutil.rmtree(dsh_dir)
    dsh_dir.mkdir(parents=True)
    npm_cache = target / "studio" / "cache" / "npm-cache"
    npm_cache.mkdir(parents=True, exist_ok=True)
    isolated_rc = target / "studio" / "cache" / "npmrc"
    isolated_rc.write_text("", encoding="ascii")  # isolate build from user's ~/.npmrc
    env = os.environ.copy()
    env.update({
        "NPM_CONFIG_CACHE": str(npm_cache),
        "NPM_CONFIG_USERCONFIG": str(isolated_rc),
        "NPM_CONFIG_UPDATE_NOTIFIER": "false",
        "NPM_CONFIG_FUND": "false",
        "NPM_CONFIG_AUDIT": "false",
    })
    if args.registry:
        env["NPM_CONFIG_REGISTRY"] = args.registry
    log("dsh", f"npm install @deepseek-ai/dsh@{args.dsh_version} (several minutes, warm cache helps)")
    proc = run([str(node_exe), str(npm_cli), "install",
                f"@deepseek-ai/dsh@{args.dsh_version}",
                "--loglevel=error"], env=env, cwd=dsh_dir)
    sys.stdout.write(proc.stdout[-500:])
    dsh_bin = dsh_dir / "node_modules" / "@deepseek-ai" / "dsh" / "lib" / "bin.js"
    if not dsh_bin.is_file():
        raise SystemExit("dsh bin.js missing after npm install")

    # ---- 4. runtime sanity probes on the package's own node ---------------
    log("probe", "node:sqlite")
    run([str(node_exe), "-e", "require('node:sqlite')"], env=os.environ)
    log("probe", "koffi native module")
    run([str(node_exe), "-e", "require('koffi')"], env=os.environ, cwd=dsh_dir)
    log("probe", "dsh --version")
    ver = run([str(node_exe), str(dsh_bin), "--version"], env=os.environ).stdout.strip()
    if ver != args.dsh_version:
        raise SystemExit(f"dsh --version says {ver}, expected {args.dsh_version}")
    log("probe", f"dsh {ver} OK")

    # ---- 5. dirs + Dsh.bat + manifest --------------------------------------
    # _home/Desktop 等.shell 常用目录: USERPROFILE 钉到 _home 后, 目录选择框
    # 解析 %USERPROFILE%\Desktop 缺目录会弹"位置不可用"
    for d in ["data/dsh", "_home/AppData/Roaming", "_home/AppData/Local",
              "_home/Temp", "_home/Desktop", "_home/Documents", "_home/Downloads",
              "studio/cache/pnpm"]:
        (target / d).mkdir(parents=True, exist_ok=True)
    bat = target / "Dsh.bat"
    bat.write_bytes(DSH_BAT_TEMPLATE.replace("\n", "\r\n").encode("ascii"))
    # bin/dsh.cmd 垫片: 让 Hermes web-ui 的 Agent 管理/Agent 预设发现包内 dsh
    # (web-ui 在进程 PATH 上找 "dsh" 命令; 垫片内置 DSH_HOME 钉在包内)
    bin_dir = target / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    (bin_dir / "dsh.cmd").write_bytes(
        ("@echo off\r\n"
         "setlocal\r\n"
         "set \"DSH_PKG=%~dp0..\"\r\n"
         "set \"DSH_HOME=%DSH_PKG%\\data\\dsh\"\r\n"
         "set \"NODE_EXE=%DSH_PKG%\\node\\node.exe\"\r\n"
         "set \"DSH_BIN=%DSH_PKG%\\dsh\\node_modules\\@deepseek-ai\\dsh\\lib\\bin.js\"\r\n"
         "if not exist \"%NODE_EXE%\" exit /b 1\r\n"
         "if not exist \"%DSH_BIN%\" exit /b 1\r\n"
         "\"%NODE_EXE%\" \"%DSH_BIN%\" %*\r\n").encode("ascii"))
    log("layout", "Dsh.bat + dirs written")

    # pre-initialize profiles/web inside the package so first boot is offline
    env_pkg = os.environ.copy()
    env_pkg["DSH_HOME"] = str(target / "data" / "dsh")
    run([str(node_exe), str(dsh_bin), "--dump-default-config", "--profile", "web"],
        env=env_pkg, check=True)
    log("layout", "profiles/web pre-initialized under data/dsh")

    manifest = {
        "schema": 1,
        "product": "dsh-portable",
        "builtAt": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "components": {
            "node": {"version": args.node_version, "sha256": actual},
            "dsh": {"version": args.dsh_version},
        },
    }
    (target / "runtime-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="ascii")

    # ---- 6. end-to-end check through Dsh.bat itself ------------------------
    proc = run(["cmd", "/c", str(bat), "--version"], env=os.environ)
    if proc.stdout.strip() != args.dsh_version:
        raise SystemExit(f"Dsh.bat --version gave {proc.stdout!r}")
    log("e2e", "Dsh.bat --version OK")

    print("\n=== DSH portable package ready ===")
    print(f"  {target}")
    print(f"  node {args.node_version} | dsh {args.dsh_version}")
    print(f"  run: {bat} web --no-open   (web UI on 127.0.0.1:3080, token in stdout)")


if __name__ == "__main__":
    main()
