#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bootstrap_qt.py —— 准备 HermesStudio 的构建环境

做两件事:
  1. pip 安装 cmake / ninja / aqtinstall
  2. 用 aqtinstall 拉 Qt 6.8.3 (MSVC2022_64) 的最小子集到 thirdparty/Qt6

为什么需要这个脚本:
  本机没有 Qt、没有 CMake。Qt 官方在线安装器是 GUI 且体积巨大,
  aqtinstall 可以命令行只拉 qtbase(+必需模块), 几分钟搞定。

已知的两个坑 (踩过, 别再踩):
  坑 1: aqt 的 -O/--outputdir 传 POSIX 路径 "/i/HermesStudio/..." 时,
        aqt 会拼出 "\\i\\HermesStudio\\..." —— Windows 把它解析成
        "当前盘符:\\i\\HermesStudio\\...", 结果装到 I:\\i\\HermesStudio\\...
        解决办法: 必须传 Windows 风格绝对路径 "I:\\HermesStudio\\thirdparty\\Qt6"
  坑 2: Qt 6 里 qtbase / qttools 是"必需模块", aqt 默认就会装。
        显式写 -m qtbase qttools 反而报
        "The packages ['qtbase','qttools'] were not found while parsing XML"
        解决办法: 不写 -m

用法:
    python scripts/bootstrap_qt.py
    python scripts/bootstrap_qt.py --qt-version 6.9.3
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENV = ROOT / "_tmp" / "venv"
QT_DIR = ROOT / "thirdparty" / "Qt6"

PIP_PACKAGES = ["cmake", "ninja", "aqtinstall"]
PIP_INDEX = "https://mirrors.aliyun.com/pypi/simple/"

# 国内镜像大多不同步 qtsdkrepository 元数据 (实测清华/阿里/腾讯/中科大均 403/404),
# 所以默认直连官方源; 若你所在网络访问 download.qt.io 困难, 可用 --base-url 覆盖。
QT_MIRRORS = [
    None,  # 官方 download.qt.io
    "https://mirrors.tuna.tsinghua.edu.cn/qt",
    "https://mirrors.aliyun.com/qt",
]


def venv_python() -> Path:
    exe = "python.exe" if os.name == "nt" else "python"
    return VENV / ("Scripts" if os.name == "nt" else "bin") / exe


def pip_install(py: Path) -> bool:
    cmd = [str(py), "-m", "pip", "install", "-q",
           "--disable-pip-version-check", "-i", PIP_INDEX, *PIP_PACKAGES]
    print(f"[bootstrap] 安装构建工具: {' '.join(PIP_PACKAGES)}")
    r = subprocess.run(cmd)
    if r.returncode != 0:
        # 镜像缺包时回退官方 PyPI
        print("[bootstrap] 镜像安装失败, 回退官方 PyPI")
        r = subprocess.run([str(py), "-m", "pip", "install", "-q",
                            "--disable-pip-version-check", *PIP_PACKAGES])
    return r.returncode == 0


def install_qt(py: Path, version: str, arch: str) -> bool:
    aqt = venv_python().parent / ("aqt.exe" if os.name == "nt" else "aqt")
    if not aqt.exists():
        print(f"[bootstrap] 找不到 aqt: {aqt}")
        return False

    # 关键: 必须是 Windows 风格绝对路径 (见文件头 坑 1)
    out = str(QT_DIR).replace("/", "\\")

    for base in QT_MIRRORS:
        cmd = [str(aqt), "install-qt", "windows", "desktop", version, arch, "-O", out]
        if base:
            cmd += ["-b", base]
        desc = base or "官方源 download.qt.io"
        print(f"[bootstrap] 下载 Qt {version} ({arch}) <- {desc}")
        r = subprocess.run(cmd)
        if r.returncode == 0 and (QT_DIR / version).exists():
            print(f"[bootstrap] Qt 就位: {QT_DIR / version}")
            return True
        print(f"[bootstrap] 该源失败, 换下一个")

    print("[bootstrap] 全部镜像均失败, 请检查网络或手动指定 --base-url")
    return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--qt-version", default="6.8.3")
    ap.add_argument("--arch", default="win64_msvc2022_64")
    ap.add_argument("--skip-qt", action="store_true", help="只装构建工具, 不下载 Qt")
    args = ap.parse_args()

    # 1) venv
    if not venv_python().exists():
        print("[bootstrap] 创建 venv ...")
        subprocess.run([sys.executable, "-m", "venv", str(VENV)], check=True)

    py = venv_python()
    if not pip_install(py):
        return 1
    print("[bootstrap] 构建工具就绪")

    if args.skip_qt:
        return 0

    # 2) Qt
    existing = sorted(QT_DIR.glob(f"*/{args.arch}"))
    if existing:
        print(f"[bootstrap] Qt 已存在, 跳过下载: {existing[0]}")
        return 0

    return 0 if install_qt(py, args.qt_version, args.arch) else 1


if __name__ == "__main__":
    raise SystemExit(main())
