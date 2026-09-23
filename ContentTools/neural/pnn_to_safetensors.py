#!/usr/bin/env python3
"""pnn_to_safetensors.py — .pnn v2 权重 → safetensors（MLX 后端部署格式）。

路线修正（Docs/2026-09-15-neural-mlx-backend-plan.md §1.3）：训练侧不变
（PyTorch），部署格式 .pnn → safetensors（业界标准，MLX 直接加载）。
本转换器**纯 stdlib**（struct/json/hashlib），不依赖 torch——权重来源就是
已导出的 .pnn（train_denoiser.py --export-pnn 的产物），可在任何机器执行。

张量命名（与 torch/MLX 训练侧约定一致，便于迁移学习加载）：
  layer{i}.weight  [out, in, k, k]  f32（torch Conv2d 原生布局）
  layer{i}.bias    [out]            f32
网络级元数据放在 __metadata__（shim 重建计算图所需）：input_dim、output_dim、
net_flags、逐层 kind/act/kernel/stride/pad/flags。

用法:
  python3 pnn_to_safetensors.py <in.pnn> <out.safetensors> [--verify]
"""
import argparse
import hashlib
import json
import struct
import sys
from datetime import date

PNN_MAGIC = 0x31574E50  # "PNW1"
ACT_NAMES = {0: "none", 1: "relu", 2: "sigmoid"}
DENSE, CONV = 0, 1


def parse_pnn(path):
    blob = open(path, "rb").read()
    (magic, version, layer_count, table_count,
     input_dim, output_dim, param_count, flags) = struct.unpack_from("<8I", blob, 0)
    if magic != PNN_MAGIC:
        sys.exit(f"bad magic 0x{magic:08X} (not a .pnn)")
    if version not in (1, 2):
        sys.exit(f"unsupported pnn version {version}")
    pos = 32
    level_res = list(struct.unpack_from(f"<{param_count}I", blob, pos)) if param_count else []
    pos += param_count * 4
    layers = []
    for _ in range(layer_count):
        if version >= 2:
            kind, i_dim, o_dim, act, k, s, p, lflags, _res = struct.unpack_from("<IIIBBBBII", blob, pos)
            pos += 24
        else:
            i_dim, o_dim = struct.unpack_from("<II", blob, pos)
            act = blob[pos + 8]
            kind, k, s, p, lflags = DENSE, 1, 1, 0, 0
            pos += 12
        layers.append(dict(kind=kind, in_dim=i_dim, out_dim=o_dim, act=act,
                           kernel=k, stride=s, pad=p, flags=lflags))
    table = list(struct.unpack_from(f"<{table_count}f", blob, pos)) if table_count else []
    pos += table_count * 4
    for lay in layers:
        kk = lay["kernel"] * lay["kernel"] if lay["kind"] == CONV else 1
        n = lay["out_dim"] * lay["in_dim"] * kk
        lay["w"] = list(struct.unpack_from(f"<{n}f", blob, pos))
        pos += n * 4
        lay["b"] = list(struct.unpack_from(f"<{lay['out_dim']}f", blob, pos))
        pos += lay["out_dim"] * 4
    if pos != len(blob):
        sys.exit(f"trailing bytes: parsed {pos} of {len(blob)}")
    return dict(version=version, input_dim=input_dim, output_dim=output_dim,
                net_flags=flags, level_res=level_res, table=table, layers=layers), blob


def write_safetensors(path, net, src_path, src_sha):
    """safetensors = u64 LE 头长 + JSON 头（data_offsets 相对数据区起点）+ f32 数据区。"""
    meta = {
        "format": "primal-pnn",
        "pnn_version": str(net["version"]),
        "input_dim": str(net["input_dim"]),
        "output_dim": str(net["output_dim"]),
        "net_flags": str(net["net_flags"]),
        "layer_count": str(len(net["layers"])),
        "source": src_path,
        "source_sha256": src_sha,
        "converted": date.today().isoformat(),
    }
    header = {"__metadata__": meta}
    tensors = []   # (name, shape, f32 list)
    for i, lay in enumerate(net["layers"]):
        if lay["kind"] == CONV:
            k = lay["kernel"]
            # pnn W[out][in][k*k] → torch [out, in, k, k]
            w = lay["w"]
            shape = [lay["out_dim"], lay["in_dim"], k, k]
        else:
            w = lay["w"]
            shape = [lay["out_dim"], lay["in_dim"]]
        tensors.append((f"layer{i}.weight", shape, w))
        tensors.append((f"layer{i}.bias", [lay["out_dim"]], lay["b"]))
        meta[f"layer{i}"] = (f"kind={'conv' if lay['kind'] == CONV else 'dense'}"
                             f",act={ACT_NAMES.get(lay['act'], 'none')}"
                             f",k={lay['kernel']},s={lay['stride']},p={lay['pad']}"
                             f",flags={'expm1' if lay['flags'] & 1 else 'none'}")
    header["__metadata__"] = meta
    offset = 0
    data = bytearray()
    for name, shape, values in tensors:
        nbytes = len(values) * 4
        header[name] = {"dtype": "F32", "shape": shape,
                        "data_offsets": [offset, offset + nbytes]}
        data += struct.pack(f"<{len(values)}f", *values)
        offset += nbytes
    hb = json.dumps(header, separators=(",", ":")).encode("utf-8")
    pad = (-len(hb)) % 8
    hb += b" " * pad   # safetensors 允许空格填充至 8 字节对齐
    with open(path, "wb") as f:
        f.write(struct.pack("<Q", len(hb)))
        f.write(hb)
        f.write(data)
    return len(tensors), 8 + len(hb) + len(data)


def verify_safetensors(path):
    """独立回读校验：头解析 + offsets 闭合 + 首尾张量抽查。"""
    blob = open(path, "rb").read()
    (n,) = struct.unpack_from("<Q", blob, 0)
    header = json.loads(blob[8:8 + n])
    data_start = 8 + n
    del header["__metadata__"]
    end_max = 0
    for name, t in header.items():
        b, e = t["data_offsets"]
        assert t["dtype"] == "F32", name
        sz = 4
        for d in t["shape"]:
            sz *= d
        assert e - b == sz, f"{name}: {e - b} != {sz}"
        end_max = max(end_max, e)
    assert data_start + end_max == len(blob), "file length mismatch"
    return len(header), end_max


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pnn")
    ap.add_argument("safetensors")
    ap.add_argument("--verify", action="store_true")
    args = ap.parse_args()
    net, blob = parse_pnn(args.pnn)
    n_tensors, total = write_safetensors(
        args.safetensors, net, args.pnn, hashlib.sha256(blob).hexdigest())
    print(f"[export] {args.safetensors} ({total} bytes, {n_tensors} tensors, "
          f"layers={len(net['layers'])} in={net['input_dim']} out={net['output_dim']} "
          f"net_flags={net['net_flags']})")
    if args.verify:
        count, sz = verify_safetensors(args.safetensors)
        print(f"[verify] {count} tensors, data {sz} bytes — OK")


if __name__ == "__main__":
    main()
