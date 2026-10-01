#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
EHsc 交叉验证脚本（可选，仅用于测试，不参与构建、不产生运行时依赖）

用 Python 标准库 hashlib / zlib 作为独立参考实现，验证 EHsc 的全部算法：
  CRC32 / MD5 / SHA-1 / SHA-256 / SHA-384 / SHA-512 / SHA3-256 / SHA3-512 /
  BLAKE2s-256 / BLAKE2b-512

覆盖内容：
  1. 各种长度（含所有块边界）与多种内容的文件哈希
  2. JSON / CSV / TXT 三种清单格式的生成与解析
  3. 清单校验（正常 / 篡改 / 缺失 / 大小变化）
  4. 字节级比较（相同 / 不同 / 长度不同）
  5. 中文路径与中文文件名

用法： python crosscheck_python.py <EHsc.exe 路径>
"""
import hashlib
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import zlib

ALGOS = [
    ("crc32", "CRC32"),
    ("md5", "MD5"),
    ("sha1", "SHA-1"),
    ("sha256", "SHA-256"),
    ("sha384", "SHA-384"),
    ("sha512", "SHA-512"),
    ("sha3_256", "SHA3-256"),
    ("sha3_512", "SHA3-512"),
    ("blake2s", "BLAKE2s-256"),
    ("blake2b", "BLAKE2b-512"),
]

# hashlib 名称 -> EHsc 清单中的算法标识
ID_MAP = {"sha3_256": "sha3-256", "sha3_512": "sha3-512"}

failures = []
checks = 0


def check(condition, message):
    global checks
    checks += 1
    if not condition:
        failures.append(message)
        print("  [FAIL] " + message)
    return condition


def reference_digest(algo, path):
    """用 Python 标准库计算参考摘要"""
    if algo == "crc32":
        crc = 0
        with open(path, "rb") as fh:
            while True:
                chunk = fh.read(1 << 20)
                if not chunk:
                    break
                crc = zlib.crc32(chunk, crc)
        return format(crc & 0xFFFFFFFF, "08x")
    h = hashlib.new(algo)
    with open(path, "rb") as fh:
        while True:
            chunk = fh.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def run(exe, args, expect_ok=True):
    proc = subprocess.run([exe] + args, capture_output=True)
    stdout = proc.stdout.decode("utf-8", errors="replace")
    stderr = proc.stderr.decode("utf-8", errors="replace")
    if expect_ok and proc.returncode != 0:
        print("  [WARN] 命令返回 %d: %s" % (proc.returncode, " ".join(args)))
    return proc.returncode, stdout, stderr


def main():
    if len(sys.argv) < 2:
        print("用法: python crosscheck_python.py <EHsc.exe>")
        return 2
    exe = os.path.abspath(sys.argv[1])
    if not os.path.isfile(exe):
        print("找不到可执行文件: " + exe)
        return 2

    workdir = tempfile.mkdtemp(prefix="ehsc_check_")
    print("工作目录: " + workdir)
    try:
        # ---------------------------------------------------------- 1. 哈希 --
        print("\n[1] 文件哈希交叉验证")
        sizes = [0, 1, 2, 3, 55, 56, 57, 63, 64, 65, 71, 72, 73, 111, 112, 113,
                 127, 128, 129, 135, 136, 137, 167, 168, 169, 200, 255, 256, 257,
                 1000, 4096, 65535, 65536, 65537, 1 << 20, (1 << 20) + 7]
        rng = random.Random(20240501)
        expected = {}
        for size in sizes:
            name = "f_%08d.bin" % size
            path = os.path.join(workdir, name)
            with open(path, "wb") as fh:
                if size > 1 << 19:
                    fh.write(bytes(rng.randrange(256) for _ in range(4096)) * (size // 4096))
                    fh.write(bytes(rng.randrange(256) for _ in range(size % 4096)))
                else:
                    fh.write(bytes(rng.randrange(256) for _ in range(size)))
            expected[name] = {algo: reference_digest(algo, path) for algo, _ in ALGOS}
        print("  已生成 %d 个测试文件（含全部块边界），参考摘要由 hashlib/zlib 计算" % len(sizes))

        out_json = os.path.join(workdir, "result.json")
        code, out, err = run(exe, ["hash", workdir, "-a", "all", "-f", "json",
                                   "-o", out_json, "--quiet", "--no-progress"])
        check(code == 0, "hash 命令返回码应为 0，实际 %d" % code)
        with open(out_json, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        check(data.get("fileCount") == len(sizes),
              "清单文件数应为 %d，实际 %s" % (len(sizes), data.get("fileCount")))

        mismatched = 0
        for entry in data["files"]:
            name = os.path.basename(entry["path"])
            want = expected.get(name)
            if want is None:
                check(False, "清单中出现未知文件: " + name)
                continue
            for algo, _display in ALGOS:
                got = entry["digests"].get(ID_MAP.get(algo, algo))
                if got != want[algo]:
                    mismatched += 1
                    check(False, "%s 的 %s 不一致: 期望 %s 实际 %s" %
                          (name, algo, want[algo], got))
        check(mismatched == 0, "共有 %d 处摘要不一致" % mismatched)
        print("  已比对 %d 个文件 × %d 种算法 = %d 个摘要" %
              (len(sizes), len(ALGOS), len(sizes) * len(ALGOS)))

        # ------------------------------------------------------ 2. 清单格式 --
        print("\n[2] 清单格式（TXT / CSV）生成")
        sub = os.path.join(workdir, "subset")
        os.makedirs(sub, exist_ok=True)
        sample = [sizes[5], sizes[16], sizes[23]]
        for size in sample:
            shutil.copy(os.path.join(workdir, "f_%08d.bin" % size),
                        os.path.join(sub, "f_%08d.bin" % size))

        for fmt, ext in (("txt", "txt"), ("csv", "csv"), ("json", "json")):
            manifest = os.path.join(workdir, "manifest." + ext)
            code, out, err = run(exe, ["hash", sub, "-a", "sha256,md5,blake2b", "-f", fmt,
                                       "-o", manifest, "--quiet", "--no-progress"])
            check(code == 0, "%s 清单生成返回码应为 0" % fmt)
            check(os.path.isfile(manifest), "%s 清单文件未生成" % fmt)
            code, out, err = run(exe, ["verify", manifest, "--no-progress"])
            check(code == 0, "%s 清单校验应通过，实际返回 %d\n%s" % (fmt, code, out))
            print("  %-4s 清单生成 + 自校验通过" % fmt)

        # ---------------------------------------------------------- 3. 校验 --
        print("\n[3] 清单校验的异常检测")
        manifest = os.path.join(workdir, "manifest.json")
        target = os.path.join(sub, "f_%08d.bin" % sample[0])
        backup = target + ".bak"
        shutil.copy(target, backup)

        # 3.1 篡改内容（长度不变）
        with open(target, "r+b") as fh:
            fh.seek(0)
            first = fh.read(1)
            fh.seek(0)
            fh.write(bytes([first[0] ^ 0xFF]))
        code, out, err = run(exe, ["verify", manifest, "--no-progress"], expect_ok=False)
        check(code == 1, "篡改文件后校验应返回 1，实际 %d" % code)
        check("不一致" in out or "Mismatch" in out, "篡改后未报告不一致")
        shutil.copy(backup, target)

        # 3.2 缺失文件
        os.remove(target)
        code, out, err = run(exe, ["verify", manifest, "--no-progress"], expect_ok=False)
        check(code == 1, "删除文件后校验应返回 1，实际 %d" % code)
        check("缺失" in out, "删除文件后未报告缺失")
        shutil.copy(backup, target)

        # 3.3 大小变化
        with open(target, "ab") as fh:
            fh.write(b"extra data appended")
        code, out, err = run(exe, ["verify", manifest, "--no-progress"], expect_ok=False)
        check(code == 1, "追加数据后校验应返回 1，实际 %d" % code)
        shutil.copy(backup, target)

        # 3.4 恢复正常
        code, out, err = run(exe, ["verify", manifest, "--no-progress"])
        check(code == 0, "恢复后校验应通过，实际 %d" % code)

        # 3.5 显式指定 --base 目录
        code, out, err = run(exe, ["verify", manifest, "--base", sub, "--no-progress"])
        check(code == 0, "使用 --base 显式指定基准目录后应通过，实际 %d" % code)

        # 3.6 清单被移动到其他位置后，仍可通过清单记录的根目录解析
        moved = os.path.join(tempfile.gettempdir(), "ehsc_moved_manifest.json")
        shutil.copy(manifest, moved)
        try:
            code, out, err = run(exe, ["verify", moved, "--no-progress"])
            check(code == 0, "清单移动到别处后应能通过记录的根目录解析，实际 %d" % code)
        finally:
            if os.path.exists(moved):
                os.remove(moved)
        print("  校验异常检测（篡改 / 缺失 / 长度变化 / --base / 清单可移动）均正确")

        # ---------------------------------------------------------- 4. 比较 --
        print("\n[4] 字节级比较")
        a = os.path.join(sub, "f_%08d.bin" % sample[0])
        b = os.path.join(sub, "f_%08d.bin" % sample[1])
        code, out, err = run(exe, ["compare", a, a, "--no-progress"])
        check(code == 0, "相同文件比较应返回 0，实际 %d" % code)
        check("完全相同" in out, "相同文件未报告完全相同")

        code, out, err = run(exe, ["compare", a, b, "--no-progress"], expect_ok=False)
        check(code == 1, "不同文件比较应返回 1，实际 %d" % code)

        # 仅在第 100 字节不同（使用 1 MiB 文件以确保长度足够）
        big = os.path.join(workdir, "f_01048576.bin")
        c = os.path.join(sub, "diff_at_100.bin")
        with open(big, "rb") as fh:
            payload = bytearray(fh.read())
        payload[100] ^= 0x01
        with open(c, "wb") as fh:
            fh.write(bytes(payload))
        code, out, err = run(exe, ["compare", big, c, "--no-progress"], expect_ok=False)
        check(code == 1, "差异文件比较应返回 1，实际 %d" % code)
        check("100" in out, "未报告首个差异偏移 100\n" + out)

        # 长度不同（公共前缀相同）
        prefix = os.path.join(sub, "prefix.bin")
        longer = os.path.join(sub, "prefix_longer.bin")
        with open(prefix, "wb") as fh:
            fh.write(b"ABCDEFGH" * 512)
        with open(longer, "wb") as fh:
            fh.write(b"ABCDEFGH" * 512 + b"tail")
        code, out, err = run(exe, ["compare", prefix, longer, "--no-progress"], expect_ok=False)
        check(code == 1, "长度不同应判定为不同，实际 %d" % code)

        # ------------------------------------------------------ 5. 中文路径 --
        print("\n[5] 中文路径与文件名")
        zh_dir = os.path.join(workdir, "中文目录")
        os.makedirs(zh_dir, exist_ok=True)
        zh_file = os.path.join(zh_dir, "测试文件-哈希校验.txt")
        with open(zh_file, "wb") as fh:
            fh.write("哈希工具中文适配测试 content".encode("utf-8") * 100)
        want = reference_digest("sha256", zh_file)
        code, out, err = run(exe, ["hash", zh_file, "-a", "sha256", "--quiet", "--no-progress"])
        check(code == 0, "中文路径哈希返回码应为 0")
        check(want in out, "中文路径的 SHA-256 不匹配\n期望 %s\n输出 %s" % (want, out))
        print("  中文文件名/目录哈希正确")

        # 中文清单往返
        zh_manifest = os.path.join(zh_dir, "清单-输出.json")
        code, out, err = run(exe, ["hash", zh_dir, "-a", "sha256,blake2s", "-f", "json",
                                   "-o", zh_manifest, "--quiet", "--no-progress"])
        check(code == 0, "中文清单写入失败")
        code, out, err = run(exe, ["verify", zh_manifest, "--no-progress"])
        check(code == 0, "中文清单校验失败，返回 %d" % code)
        print("  中文清单生成 + 校验通过")

        # ------------------------------------------------------ 6. 回归测试 --
        print("\n[6] 回归测试（历史缺陷）")
        # 6.1 目标路径带尾部反斜杠时，清单必须保留完整目录结构
        tree = os.path.join(workdir, "tree")
        os.makedirs(os.path.join(tree, "sub", "deep"), exist_ok=True)
        with open(os.path.join(tree, "top.txt"), "wb") as fh:
            fh.write(b"top")
        with open(os.path.join(tree, "sub", "nested.txt"), "wb") as fh:
            fh.write(b"nested")
        with open(os.path.join(tree, "sub", "deep", "x.bin"), "wb") as fh:
            fh.write(b"deep")
        for variant, label in ((tree + os.sep, "带尾反斜杠"), (tree, "无尾反斜杠")):
            manifest = os.path.join(workdir, "tree_%s.csv" % ("trail" if label[0] == "带" else "plain"))
            code, out, err = run(exe, ["hash", variant, "-f", "csv", "-o", manifest,
                                       "--quiet", "--no-progress"])
            check(code == 0, "[%s] 生成清单失败" % label)
            with open(manifest, "r", encoding="utf-8") as fh:
                content = fh.read()
            check("sub\\nested.txt" in content or "sub/nested.txt" in content,
                  "[%s] 清单丢失了子目录结构：\n%s" % (label, content))
            code, out, err = run(exe, ["verify", manifest, "--no-progress"])
            check(code == 0, "[%s] 清单自校验应通过，实际 %d\n%s" % (label, code, out))
        print("  尾反斜杠/盘根路径的目录结构保留正确")

        # 6.2 读取失败的文件必须保留在清单中（CSV/JSON），不能凭空消失
        locked = os.path.join(tree, "locked.bin")
        with open(locked, "wb") as fh:
            fh.write(b"will be locked")
        with open(locked, "rb"):
            pass
        # Windows 下用独占打开模拟“无法读取”
        import ctypes
        from ctypes import wintypes
        GENERIC_READ, OPEN_EXISTING = 0x80000000, 3
        handle = ctypes.windll.kernel32.CreateFileW(
            ctypes.c_wchar_p(locked), GENERIC_READ, 0, None, OPEN_EXISTING, 0, None)
        if handle == -1 or handle == 0xFFFFFFFFFFFFFFFF:
            print("  (无法独占锁定文件，跳过 6.2)")
        else:
            try:
                manifest = os.path.join(workdir, "locked.csv")
                code, out, err = run(exe, ["hash", locked, "-f", "csv", "-o", manifest,
                                           "--quiet", "--no-progress"], expect_ok=False)
                with open(manifest, "r", encoding="utf-8") as fh:
                    content = fh.read()
                check("locked.bin" in content,
                      "读取失败的文件必须仍出现在清单里（否则缺失永远无法被发现）：\n" + content)
                check("Note" in content.splitlines()[2] or "Note" in content,
                      "读取失败时 CSV 应包含 Note 列说明原因")
                code, out, err = run(exe, ["verify", manifest, "--no-progress"], expect_ok=False)
                check("跳过" in out or "不一致" in out or "缺失" in out,
                      "校验含失败条目的清单时应有明确提示\n" + out)
                print("  读取失败的条目仍保留在清单中并可被校验提示")
            finally:
                ctypes.windll.kernel32.CloseHandle(handle)

        # ------------------------------------------------------ 7. 退出码 --
        print("\n[7] 自检退出码")
        code, out, err = run(exe, ["selftest"])
        check(code == 0, "selftest 应返回 0，实际 %d" % code)
        check("321" in out, "自检项数异常")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    print("\n" + "=" * 60)
    if failures:
        print("交叉验证失败：%d / %d 项未通过" % (len(failures), checks))
        for item in failures[:20]:
            print("  - " + item)
        return 1
    print("交叉验证全部通过：%d 项检查，含 %d 个摘要比对" % (checks, len(sizes) * len(ALGOS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
