#!/usr/bin/env python3
"""compare_f350.py — r5 第 350 帧三组对比工具 (改造自 r4 的 compare_ppm.py)。

输入若干 PPM (P6, maxval=255), 第 1 个视为 Mac 参照; 输出:
  - 全部无序对的 PSNR / SSIM / 均值绝对差 / 最大绝对差 / 逐字节相等与否
    (参照 vs 每个候选 + 候选之间两两互比, n*(n-1)/2 对);
  - JSON 明细到 stdout, 可选 --json FILE 落盘;
  - 末尾人读汇总表。

指标口径 (与 r4 compare_ppm.py 的差异已逐条注明):
  - psnr_rgb_db / psnr_r_g_b_db / psnr_luma709_db: 与 r4 逐字节同式
    (MSE over packed bytes / 每通道 / Rec.709 整数 luma (54,183,19)//256)。
    噪声地板参照: 同一 Mac 二进制重跑两次, r4 实测 psnr_rgb_db=45.2874。
  - mean_abs_diff_rgb / max_abs_diff_rgb / mean_abs_diff_luma / max_abs_diff_luma:
    与 r4 同式。
  - byte_identical: 对像素数据做 sha256 比对 (同 r4)。
  - ssim_luma_8x8block: 纯标准库实现 (r4 依赖 numpy, Windows 复测机可能没有,
    本版删掉 numpy 分支, Mac/Windows 走同一条代码路径、数字逐位一致)。
    精度口径 (如实声明): 灰度 Rec.709 整数 luma 上的 8x8 **不重叠**均匀方块
    SSIM (1280x720 -> 160x90 块取均值), C1=(0.01*255)^2, C2=(0.03*255)^2。
    原始 Wang-Bovik 与仓库 EngineTest/Utils/ImageCompare::ComputeSSIM 用的是
    8x8 滑动窗 + 高斯(sigma=1.5) — 数值不可与那些口径直接互比, 本字段只用于
    本脚本产出的各对比对之间的相对比较。
  - PSNR=Infinity 时 JSON 里保持 Infinity 字面量 (Python json 可读, 非 strict
    JSON; 与 r4 行为一致)。

用法 (r5 默认示例, Windows):
  rem 组1(d3d12 GI-on)/组2(gioff 两帧)/组3(win-vulkan GI-on) 各自 vs Mac 参照
  rem + 两两互比 (d3d12 vs win-vulkan, gioff 对 gioff 等):
  python compare_f350.py reference_vulkan_mac_f350_after_r4edits.ppm ^
    r5_out\\art2_win_d3d12_f350.ppm ^
    r5_out\\art2_win_d3d12_gioff_f350.ppm ^
    r5_out\\art2_win_vulkan_gioff_f350.ppm ^
    r5_out\\art2_win_vulkan_f350.ppm --json r5_out\\compare_f350.json

Mac 侧 (python3, 参照重建自证):
  python3 compare_f350.py /path/to/reference_vulkan_mac_f350_after_r4edits.ppm \
    /tmp/art2.ppm            # 期望 psnr_rgb_db ~45dB 量级 (噪声地板)
  自比 (同文件传两次) 期望 byte_identical=true, PSNR=Infinity, SSIM=1.0。

仅依赖 Python 3 标准库 (sys/json/math/hashlib/itertools)。
"""
import sys, json, math, hashlib
from itertools import combinations

# --- SSIM 常数 (Wang-Bovik 标准): C1=(0.01*255)^2, C2=(0.03*255)^2 ---
SSIM_C1 = (0.01 * 255) ** 2
SSIM_C2 = (0.03 * 255) ** 2
SSIM_BLOCK = 8


def read_p6(path):
    """读 P6 PPM。header 解析沿用 r4 compare_ppm.py (空白/注释容忍, maxval=255)。"""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise SystemExit(f"{path}: not a P6 PPM")
    i, fields = 2, []
    while len(fields) < 3:
        while i < len(data) and data[i:i+1].isspace():
            i += 1
        if data[i:i+1] == b"#":  # 注释行
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j+1].isspace():
            j += 1
        fields.append(int(data[i:j])); i = j
    i += 1  # maxval 后单个空白
    w, h, maxv = fields
    if maxv != 255:
        raise SystemExit(f"{path}: only maxval=255 supported (got {maxv})")
    px = data[i:i + w * h * 3]
    if len(px) != w * h * 3:
        raise SystemExit(f"{path}: truncated pixel data ({len(px)} of {w*h*3})")
    return w, h, px


def luma709_plane(px):
    """Rec.709 整数 luma (与 r4 luma PSNR 同一权重 54/183/19 // 256)。"""
    out = [0] * (len(px) // 3)
    o = 0
    for p in range(len(out)):
        out[p] = (54 * px[o] + 183 * px[o+1] + 19 * px[o+2]) // 256
        o += 3
    return out


def block_moments(plane, w, h, block=SSIM_BLOCK):
    """8x8 不重叠方块的 (sum, sum_sq) 列表, 行主序, 尾部不满块丢弃。"""
    bh, bw = h // block, w // block
    moments = []
    for by in range(bh):
        y0 = by * block
        for bx in range(bw):
            x0 = bx * block
            s = sq = 0
            for y in range(y0, y0 + block):
                base = y * w + x0
                for x in range(base, base + block):
                    v = plane[x]
                    s += v
                    sq += v * v
            moments.append((s, sq))
    return moments


def psnr_from_se(se, count):
    if se == 0:
        return float("inf")
    return 10 * math.log10(255 ** 2 / (se / count))


def compare_pair(img_a, img_b):
    """img_*: dict(w,h,px,sha,luma,blocks)。返回 r4 兼容键 + 新 ssim 键。"""
    w, h = img_a["w"], img_a["h"]
    a, b = img_a["px"], img_b["px"]
    n = len(a)

    if img_a["sha"] == img_b["sha"]:  # 逐字节相等: 全部差值指标恒 0, 短路
        return {
            "byte_identical": True,
            "psnr_rgb_db": float("inf"),
            "psnr_r_g_b_db": [float("inf")] * 3,
            "psnr_luma709_db": float("inf"),
            "mean_abs_diff_rgb": 0.0,
            "max_abs_diff_rgb": 0,
            "mean_abs_diff_luma": 0.0,
            "max_abs_diff_luma": 0,
            "ssim_luma_8x8block": 1.0,
        }

    # PSNR / MAE: 与 r4 逐字节同式 (packed + 每通道 + luma)
    se = [0, 0, 0]; se_all = 0; sae_all = 0; maxd = 0
    for k in range(n):
        d = a[k] - b[k]
        ad = d if d >= 0 else -d
        sae_all += ad
        if ad > maxd:
            maxd = ad
        d2 = d * d
        se_all += d2
        se[k % 3] += d2
    psnr_all = psnr_from_se(se_all, n)
    psnr_ch = [psnr_from_se(se[c], n // 3) for c in range(3)]

    npix = w * h
    la, lb = img_a["luma"], img_b["luma"]
    se_l = sae_l = 0; maxd_l = 0
    for p in range(npix):
        d = la[p] - lb[p]
        ad = d if d >= 0 else -d
        sae_l += ad
        if ad > maxd_l:
            maxd_l = ad
        se_l += d * d
    psnr_l = psnr_from_se(se_l, npix)

    # SSIM: 8x8 不重叠方块, 均匀窗; per-image (sum,sum_sq) 已缓存,
    # 逐对只需重算协方差项 sum(a*b) (按块访问 luma 平面)。
    m_a, m_b = img_a["blocks"], img_b["blocks"]
    if len(m_a) != len(m_b):
        raise SystemExit("internal: block count mismatch")
    k = 64  # 每块像素数 (8x8)
    ssim_sum = 0.0
    bh, bw = h // SSIM_BLOCK, w // SSIM_BLOCK
    for by in range(bh):
        y0 = by * SSIM_BLOCK
        for bx in range(bw):
            x0 = bx * SSIM_BLOCK
            sab = 0
            for y in range(y0, y0 + SSIM_BLOCK):
                base = y * w + x0
                for x in range(base, base + SSIM_BLOCK):
                    sab += la[x] * lb[x]
            # 取该块的 (sa,sqa)/(sb,sqb) —— 行主序索引
            idx = by * bw + bx
            sa, sqa = m_a[idx]
            sb, sqb = m_b[idx]
            mu_a = sa / k
            mu_b = sb / k
            var_a = sqa / k - mu_a * mu_a
            var_b = sqb / k - mu_b * mu_b
            cov = sab / k - mu_a * mu_b
            ssim_sum += ((2 * mu_a * mu_b + SSIM_C1) * (2 * cov + SSIM_C2)) / \
                        ((mu_a * mu_a + mu_b * mu_b + SSIM_C1) * (var_a + var_b + SSIM_C2))
    ssim = ssim_sum / (bh * bw)

    return {
        "byte_identical": False,
        "psnr_rgb_db": round(psnr_all, 4),
        "psnr_r_g_b_db": [round(p, 4) for p in psnr_ch],
        "psnr_luma709_db": round(psnr_l, 4),
        "mean_abs_diff_rgb": round(sae_all / n, 6),
        "max_abs_diff_rgb": maxd,
        "mean_abs_diff_luma": round(sae_l / npix, 6),
        "max_abs_diff_luma": maxd_l,
        "ssim_luma_8x8block": round(ssim, 6),
    }


def load_image(path):
    w, h, px = read_p6(path)
    sha = hashlib.sha256(px).hexdigest()
    luma = luma709_plane(px)
    blocks = block_moments(luma, w, h)
    return {"path": path, "label": path.replace("\\", "/").rsplit("/", 1)[-1],
            "w": w, "h": h, "px": px, "sha": sha, "luma": luma, "blocks": blocks}


def main():
    # 位置参数 = PPM 路径; --json FILE 的 FILE 不是路径
    argv = sys.argv[1:]
    args, i = [], 0
    while i < len(argv):
        if argv[i] == "--json":
            i += 2  # 跳过 flag 与其值
            continue
        if argv[i].startswith("--"):
            raise SystemExit(f"unknown flag: {argv[i]}")
        args.append(argv[i])
        i += 1
    if len(args) < 2:
        raise SystemExit(__doc__)
    images = []
    for p in args:
        img = load_image(p)
        img["size"] = f"{img['w']}x{img['h']}"
        images.append(img)

    out = {
        "reference": {"path": images[0]["path"], "sha256_pixels": images[0]["sha"],
                      "size": images[0]["size"]},
        "inputs": [{"path": im["path"], "sha256_pixels": im["sha"],
                    "size": im["size"]} for im in images],
        "pairs": [],
    }
    errors = []
    for i, j in combinations(range(len(images)), 2):
        a, b = images[i], images[j]
        pair = {"a": a["label"], "b": b["label"],
                "a_is_reference": i == 0}
        if (a["w"], a["h"]) != (b["w"], b["h"]):
            pair["error"] = f"size mismatch: {a['size']} vs {b['size']}"
            errors.append(pair)
        else:
            pair.update(compare_pair(a, b))
        out["pairs"].append(pair)

    print(json.dumps(out, indent=2))
    if "--json" in sys.argv:
        out_path = sys.argv[sys.argv.index("--json") + 1]
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=2)

    # 人读汇总表
    print()
    print("=" * 100)
    print(f"{'pair':<66}{'PSNRrgb':>9}{'PSNRluma':>9}{'SSIM8x8':>9}{'MADrgb':>9}{'MAXD':>6}{'ident':>7}")
    print("-" * 100)
    for p in out["pairs"]:
        if "error" in p:
            print(f"{p['a']} vs {p['b']}: ERROR {p['error']}")
            continue
        ps = p["psnr_rgb_db"]
        pl = p["psnr_luma709_db"]
        ps_s = "inf" if ps == float("inf") else f"{ps:.3f}"
        pl_s = "inf" if pl == float("inf") else f"{pl:.3f}"
        tag = f"{p['a']} vs {p['b']}"
        if len(tag) > 65:
            tag = tag[:62] + "..."
        print(f"{tag:<66}{ps_s:>9}{pl_s:>9}{p['ssim_luma_8x8block']:>9.4f}"
              f"{p['mean_abs_diff_rgb']:>9.3f}{p['max_abs_diff_rgb']:>6}{'YES' if p['byte_identical'] else 'no':>7}")
    print("=" * 100)
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
