#!/usr/bin/env python3
"""triage_capture.py — 从 capture 目录自动分拣出 package-sr 需要的工件。

原理：package_dlss_sr.py 对权重 blob（3,545,824 B）和重建滤波器（65,536 B）
有硬 SHA 校验（WEIGHTS_SHA256/FILTER_SHA256），而这两个 blob 藏在 capture 的
哪个 launch 里取决于运行时 launch 顺序。本脚本遍历所有 L*_buf*.bin，
按内容 SHA 找到它们，再把对应的 launch 重命名成 package-sr 期望的
L0001（权重，params 块 176 B）/ L0012（滤波器，params 块 328 B），
并校验参数块内指针偏移（权重在 param 64、滤波器在 param 280）。

用法:
    python3 triage_capture.py cap/ out/
成功后 out/ 下即有 package-sr 直接可用的 capture 目录：
    mlxdlss-weights package-sr libnvidia-ngx-dlss.so.310.7.0 out/ dlss-sr.srmodel
"""
import hashlib
import os
import re
import shutil
import struct
import sys

WEIGHTS_SHA256 = "ddaa59cd82deed53bad9d65b82b454f1439fc4875e4220b3ffafafb6cd0296a1"
WEIGHTS_SIZE = 3_545_824
WEIGHTS_PARAM_OFFSET = 64
WEIGHTS_PARAMS_LEN = 176

FILTER_SHA256 = "88aa97aa1e2cb9e2e7c932476b24793a4fb998a97c76c3b351212224bc00ab3c"
FILTER_SIZE = 65_536
FILTER_PARAM_OFFSET = 280
FILTER_PARAMS_LEN = 328


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    cap_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)

    found = {"weights": None, "filter": None}
    for name in sorted(os.listdir(cap_dir)):
        m = re.fullmatch(r"(L\d+)_buf([0-9a-f]+)\.bin", name)
        if not m:
            continue
        path = os.path.join(cap_dir, name)
        size = os.path.getsize(path)
        if size not in (WEIGHTS_SIZE, FILTER_SIZE):
            continue
        digest = sha256(path)
        if digest == WEIGHTS_SHA256:
            found["weights"] = (m.group(1), path, m.group(2))
            print(f"[hit] WEIGHTS blob: {name}")
        elif digest == FILTER_SHA256:
            found["filter"] = (m.group(1), path, m.group(2))
            print(f"[hit] FILTER blob: {name}")

    ok = True
    for kind, hit in found.items():
        if hit is None:
            print(f"[MISS] {kind}: 未找到（大小 {WEIGHTS_SIZE if kind == 'weights' else FILTER_SIZE} "
                  f"且 SHA 匹配的 buffer）——见 nvcuda_capture.c 头部'已知边界'")
            ok = False
            continue
        launch, path, base = hit

    if not ok:
        return 1

    # 权重 launch → L0001；滤波器 launch → L0012。同名 launch 冲突时报警。
    if found["weights"][0] == found["filter"][0]:
        print("[FAIL] 权重与滤波器落在同一 launch（异常）")
        return 1

    for kind, (launch, src, base) in found.items():
        target = "0001" if kind == "weights" else "0012"
        # 对应的 params 块必须存在且长度/指针偏移符合 package-sr 预期
        params = os.path.join(cap_dir, f"{launch}_params.bin")
        exp_len = WEIGHTS_PARAMS_LEN if kind == "weights" else FILTER_PARAMS_LEN
        exp_off = WEIGHTS_PARAM_OFFSET if kind == "weights" else FILTER_PARAM_OFFSET
        data = open(params, "rb").read()
        if len(data) != exp_len:
            print(f"[WARN] {kind} params {launch}_params.bin 长度 {len(data)} ≠ 预期 "
                  f"{exp_len}——如 package-sr 拒收，需检查参数块线性化（退化路径?）")
        ptr = struct.unpack_from("<Q", data, exp_off)[0] if len(data) >= exp_off + 8 else 0
        base_int = int(base, 16)
        if not (base_int <= ptr < base_int + os.path.getsize(src)):
            print(f"[FAIL] {kind} params@{exp_off} 指针 0x{ptr:x} 不在 buffer "
                  f"0x{base_int:x} 范围内")
            ok = False
            continue
        # 重命名该 launch 的全部文件
        for name in os.listdir(cap_dir):
            if name.startswith(launch + "_"):
                new = target + name[len(launch):]
                shutil.copy2(os.path.join(cap_dir, name), os.path.join(out_dir, new))
        print(f"[ok] {kind}: {launch} → L{target}（buf 基址 0x{base_int:x}）")

    if not ok:
        return 1
    print(f"[done] {out_dir}/ 即 package-sr 的 CAPTURE_DIR：\n"
          f"  mlxdlss-weights package-sr libnvidia-ngx-dlss.so.310.7.0 {out_dir}/ dlss-sr.srmodel")
    return 0


if __name__ == "__main__":
    sys.exit(main())
