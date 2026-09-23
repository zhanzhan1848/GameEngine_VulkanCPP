#!/usr/bin/env python
"""train_nr.py — R3.2 NR transformer 微调（torch/MPS 参考图 + 冻结主干）。

设计（Docs/2026-09-22-neural-rendering-inference-training-plan.md §R3.2）：
  数据   = 采集 take npz（7ch GI 特征 + GT mean）
  适配   = 方案 B：in_proj（7→6 的 1×1 conv，产出 vendor ch4-9 两份颜色通道）
           + vendor 固定通道（0-2 确定性噪声、3 常数、10-15 控制位）拼成 16ch。
           in_proj 初始化 = 方案 C 适配（tonemap 颜色 ×0.125 中心化，双副本），
           使零训练起点等价 nr_adapter.py 的推理适配。
  冻结   = 主干 71 块全冻结（buffers 无梯度）；可训练 = in_proj +
           block0.layer0.input_adapter_weight + block70 输出头（out_gain /
           out_conv_weight）。
  损失   = L1 + SSIM（LDR [0,1] 域，1-exp(-1.6c) tonemap，与 R1.2 评估一致）。
  输出   = 微调后完整 logical safetensors（649 张量，可训练项被替换），
           可经 package_neural_rendering_transformer.py 重建 .dlssmodel 包。

用法:
  .venv/bin/python ContentTools/neural/train_nr.py /tmp/neural_capture_m2 \
      --train 0,1,2 --test 3 --steps 300
"""
import argparse
import os
import sys

import numpy as np
import torch
import torch.nn as nn

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MLX_DLSS_PY = "/Users/zhanyuanwei/Desktop/MLX-DLSS/python"
DEFAULT_LOGICAL = f"{REPO}/weights/nr/dlssnr-weights-logical.safetensors"

# 默认冻结 vendor 输出头（block70 head 动了会把残差语义推离预训练工作点，
# 2026-09-23 实测：400 步 lr2e-4 全开 → holdout 53.15→23.77dB 坍塌）。
TRAINABLE_WEIGHTS = (
    "block0.layer0.input_adapter_weight",
)


def load_take_feats(d, idx, tag="sponza"):
    """7ch 输入（tonemap 颜色 + normal + depth/16）+ LDR GT。"""
    z = np.load(os.path.join(d, f"dataset_take_{tag}_{idx}.npz"))
    noisy = z["gi_noisy"].astype(np.float32)
    color01 = 1.0 - np.exp(-noisy * np.float32(1.6))
    color01 = color01.clip(0.0, 1.0).astype(np.float32)
    feats = np.zeros(color01.shape[:2] + (7,), np.float32)
    feats[..., 0:3] = color01
    feats[..., 3:6] = z["normal"].astype(np.float32)
    feats[..., 6] = np.clip(z["depth_lin"].astype(np.float32) / 16.0, 0, 1)
    gt01 = (1.0 - np.exp(-z["gt_mean"].astype(np.float32) * np.float32(1.6))).clip(0, 1)
    return feats, gt01.astype(np.float32)


class NRAdapterInput(nn.Module):
    """7ch GI → vendor 16ch：in_proj 出 ch4-9，其余固定 vendor 语义通道。"""

    def __init__(self):
        super().__init__()
        self.proj = nn.Conv2d(7, 6, 1, bias=True)
        # 方案 C 等价初始化：ch4-6 = (c01-0.5)*0.125（0.125·onehot - 0.0625），
        # ch7-9 同副本；normal/depth 权重 0（微调学注入）。
        w = torch.zeros(6, 7, 1, 1)
        for c in range(3):
            w[c, c, 0, 0] = 0.125
            w[c + 3, c, 0, 0] = 0.125
        self.proj.weight.data = w
        self.proj.bias.data = torch.full((6,), -0.0625)

    def forward(self, x7chw, noise_chw):
        n, _, h, w = x7chw.shape
        color6 = self.proj(x7chw)                      # [n,6,h,w]
        out = x7chw.new_zeros((n, 16, h, w))
        out[:, 0:3] = noise_chw
        out[:, 3] = 1.0
        out[:, 4:10] = color6
        out[:, 10] = 0.0
        out[:, 11] = 1.0
        out[:, 12] = 1.0
        out[:, 13] = -1.0
        out[:, 14] = -1.0
        out[:, 15] = 0.0
        return out


def make_trainable(model: nn.Module, names):
    """把指定 logical 权重从 buffer 换成 Parameter（同名 attribute，可训练）。"""
    made = []
    for logical, attr in model._weight_attributes.items():
        if logical in names:
            buf = getattr(model, attr)
            param = nn.Parameter(buf.detach().clone())
            delattr(model, attr)
            model.register_parameter(attr, param)
            made.append(logical)
    missing = set(names) - set(made)
    if missing:
        raise ValueError(f"trainable weights not found: {sorted(missing)}")
    return made


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--tag", default="sponza")
    ap.add_argument("--weights", default=DEFAULT_LOGICAL)
    ap.add_argument("--train", default="0,1,2")
    ap.add_argument("--test", default="3")
    ap.add_argument("--steps", type=int, default=300)
    ap.add_argument("--crop", type=int, default=256)
    ap.add_argument("--batch", type=int, default=2)
    ap.add_argument("--lr", type=float, default=1e-5)
    ap.add_argument("--train-head", action="store_true",
                    help="连 block70 输出头一起训（默认冻结；易坍塌，见上）")
    ap.add_argument("--frame-index", type=int, default=0)
    ap.add_argument("--export", default="")
    ap.add_argument("--eval-only", action="store_true")
    args = ap.parse_args()

    sys.path.insert(0, MLX_DLSS_PY)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from mlxdlss.model import NeuralRenderingModel
    from nr_adapter import deterministic_noise
    from metrics import psnr, ssim

    dev = "mps" if torch.backends.mps.is_available() else "cpu"
    torch.manual_seed(11)

    print(f"[data] {args.dir} tag={args.tag}")
    train_ids = [int(x) for x in args.train.split(",") if x != ""]
    test_ids = [int(x) for x in args.test.split(",") if x != ""]
    data = {}
    for i in sorted(set(train_ids + test_ids)):
        f, g = load_take_feats(args.dir, i, args.tag)
        data[i] = (f, g)
        print(f"  take {i}: {f.shape}")

    print(f"[model] loading {args.weights} ...")
    model = NeuralRenderingModel.from_safetensors(args.weights) \
        if hasattr(NeuralRenderingModel, "from_safetensors") else None
    if model is None:
        from safetensors.torch import load_file
        model = NeuralRenderingModel(load_file(args.weights))
    model = model.to(dev).eval()

    adapter = NRAdapterInput().to(dev)
    wanted = TRAINABLE_WEIGHTS if not args.train_head else TRAINABLE_WEIGHTS + (
        "block70.layer0.out_gain", "block70.layer0.out_conv_weight")
    trainable_made = make_trainable(model, wanted)
    print(f"[model] trainable: {trainable_made} + in_proj")
    params = list(adapter.parameters()) + [
        p for n, p in model.named_parameters() if "weight_" in n]
    print(f"[model] trainables={sum(p.numel() for p in params)}")

    crop = args.crop

    def rand_batch():
        xs, ys = [], []
        for _ in range(args.batch):
            i = train_ids[np.random.randint(len(train_ids))]
            f, g = data[i]
            h, w = f.shape[:2]
            y = np.random.randint(0, h - crop)
            x = np.random.randint(0, w - crop)
            xs.append(f[y:y + crop, x:x + crop])
            ys.append(g[y:y + crop, x:x + crop])
        x = torch.from_numpy(np.stack(xs).transpose(0, 3, 1, 2)).to(dev)
        y = torch.from_numpy(np.stack(ys)).to(dev)
        return x, y

    def run_crops(x7):
        """训练路径：batch 7ch crop（%64）→ 16ch → head 残差输出 [n,h,w,3]。"""
        noise = deterministic_noise(x7.shape[2], x7.shape[3], args.frame_index)
        noise_chw_t = torch.from_numpy(noise.transpose(2, 0, 1)).to(dev)
        feats16 = adapter(x7, noise_chw_t.unsqueeze(0).expand(x7.shape[0], -1, -1, -1))
        feats16 = feats16.permute(0, 2, 3, 1)   # NHWC 给参考图
        head = model(feats16)                    # [n,h,w,4]
        color01 = x7[:, 0:3].permute(0, 2, 3, 1)
        return (color01 + 0.25 * head[..., :3]).clamp(0, 1)

    def run_full(f7):
        """评估路径：整帧镜像对齐到 64 倍数 → 网络 → 裁回。"""
        from nr_adapter import NetworkGeometry
        h, w = f7.shape[:2]
        geo = NetworkGeometry.vendor_aligned(w, h)
        ext = geo.extend(f7)                     # (nH, nW, 7)
        x = torch.from_numpy(ext.transpose(2, 0, 1)[None]).to(dev)
        out = run_crops(x)[0].cpu().numpy()
        return out[:h, :w]

    def evaluate(tag_ids, label):
        model.eval()
        results = []
        with torch.no_grad():
            for i in tag_ids:
                f, g = data[i]
                out = run_full(f)
                results.append((psnr(out, g, 1.0), ssim(out, g)))
        p = float(np.mean([r[0] for r in results]))
        s = float(np.mean([r[1] for r in results]))
        print(f"[eval {label}] PSNR={p:.2f} dB SSIM={s:.5f} "
              + " ".join(f"(take{i}:{r0:.2f}/{r1:.4f})" for i, (r0, r1) in zip(tag_ids, results)))
        return p, s

    p0, s0 = evaluate(test_ids, "zero-train(方案C起点)")
    if args.eval_only:
        return 0

    opt = torch.optim.AdamW(params, lr=args.lr)
    ckpt_path = os.path.join(args.dir, "nr_finetune_ckpt.pt")

    def ssim_loss(x, y):
        # 简化全局 SSIM（训练正则项；门禁评估用 metrics.ssim）
        mx, my = x.mean(), y.mean()
        vx, vy = x.var(), ((x - mx) * (y - my)).mean()
        c1, c2 = 0.01 ** 2, 0.03 ** 2
        return -((2 * mx * my + c1) * (2 * vx + c2)
                 / (mx * mx + my * my + c1) / (vx + vy + c2) + 1.0)

    model.train()
    import time
    train_start = time.time()
    for step in range(args.steps):
        x, y = rand_batch()
        pred = run_crops(x)
        loss = torch.mean(torch.abs(pred - y)) + 0.1 * ssim_loss(pred, y)
        opt.zero_grad(set_to_none=True)
        loss.backward()
        opt.step()
        if step % 20 == 0 or step == args.steps - 1:
            rate = (time.time() - train_start) / (step + 1)
            print(f"[train] step {step:4d} loss {loss.item():.5f} ({rate:.1f}s/step)",
                  flush=True)

    torch.save({"step": args.steps - 1}, ckpt_path)   # 占位：参数经 export 导出
    p1, s1 = evaluate(test_ids, "fine-tuned")
    print(f"[gate] PSNR {p0:.2f} -> {p1:.2f} dB (gate >= 30) | "
          f"SSIM {s0:.5f} -> {s1:.5f}")

    if args.export:
        from safetensors.torch import load_file, save_file
        all_weights = load_file(args.weights)
        for logical in trainable_made:
            attr = model._weight_attributes[logical]
            all_weights[logical] = getattr(model, attr).detach().cpu().to(torch.float32)
        save_file(all_weights, args.export,
                  metadata={"format": "dlssnr-logical-v16", "fully_logical": "true"})
        print(f"[export] {args.export}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
