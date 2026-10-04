#!/usr/bin/env python3
"""
固件镜像与代码资产凭据扫描工具。
扫描指定的二进制镜像（如 merged-binary.bin）或文本文件中是否存在指定的禁用敏感字符串。
当发现任何匹配敏感字符串时以退出码 1 退出，否则以退出码 0 正常退出。
"""

import argparse
import os
import sys
from pathlib import Path
from typing import List, Tuple


def load_patterns_from_file(patterns_file: str) -> List[str]:
    """从模式文件中加载待检查的敏感字符串列表，过滤空行和注释。"""
    path = Path(patterns_file)
    if not path.is_file():
        raise FileNotFoundError(f"Patterns file not found: {patterns_file}")

    patterns = []
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            stripped = line.strip()
            if stripped and not stripped.startswith("#"):
                patterns.append(stripped)
    return patterns


def scan_bytes(data: bytes, patterns: List[str]) -> List[str]:
    """在字节序列中检索敏感字符串列表，返回所有命中的敏感字符串。"""
    matches = []
    for pattern in patterns:
        if not pattern:
            continue
        pattern_bytes = pattern.encode("utf-8")
        if pattern_bytes in data:
            matches.append(pattern)
    return matches


def scan_file(target_file: str, patterns: List[str]) -> Tuple[bool, List[str]]:
    """扫描目标单文件，返回 (is_clean, matches)。"""
    target = Path(target_file)
    if not target.is_file():
        raise FileNotFoundError(f"Target file not found: {target_file}")

    data = target.read_bytes()
    matches = scan_bytes(data, patterns)
    is_clean = len(matches) == 0
    return is_clean, matches


def scan_target(target_path: str, patterns: List[str]) -> Tuple[bool, List[Tuple[str, str]]]:
    """扫描目标文件或目录，返回 (is_clean, [(filepath, match), ...])。"""
    target = Path(target_path)
    if not target.exists():
        raise FileNotFoundError(f"Target not found: {target_path}")

    files_to_scan = []
    if target.is_file():
        files_to_scan.append(target)
    elif target.is_dir():
        for root, _, files in os.walk(target):
            for f in files:
                files_to_scan.append(Path(root) / f)

    all_matches = []
    for file_path in files_to_scan:
        try:
            _, file_matches = scan_file(str(file_path), patterns)
            for m in file_matches:
                all_matches.append((str(file_path), m))
        except Exception:
            continue

    return len(all_matches) == 0, all_matches


def main() -> int:
    parser = argparse.ArgumentParser(
        description="扫描二进制固件或文件中是否包含敏感凭据信息"
    )
    parser.add_argument(
        "target",
        help="待扫描的目标文件或目录路径 (例如 build/merged-binary.bin 或 dist/firmware_package)",
    )
    parser.add_argument(
        "--patterns-file",
        "-p",
        help="包含敏感凭据清单的文件路径 (每行一条，支持 # 注释)",
    )
    parser.add_argument(
        "--pattern",
        action="append",
        default=[],
        help="显式指定待扫描的单个敏感字符串 (可多次指定)",
    )

    args = parser.parse_args()

    patterns = list(args.pattern)
    if args.patterns_file:
        patterns.extend(load_patterns_from_file(args.patterns_file))

    if not patterns:
        print("[WARN] 未提供任何待扫描的敏感字符串模式，扫描通过。")
        return 0

    try:
        is_clean, matches = scan_target(args.target, patterns)
    except Exception as e:
        print(f"[ERROR] 扫描执行异常: {e}", file=sys.stderr)
        return 2

    if not is_clean:
        print(f"[FAIL] 扫描失败！在 '{args.target}' 中检测到 {len(matches)} 处敏感凭据！", file=sys.stderr)
        for filepath, m in matches:
            redacted = m[:2] + "***" + m[-1:] if len(m) > 3 else "***"
            print(f"  - [{filepath}] 命中规则: [长度 {len(m)}] {redacted}", file=sys.stderr)
        return 1

    print(f"[PASS] 扫描通过！'{args.target}' 中未发现任何指定凭据。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
