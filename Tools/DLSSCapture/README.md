# DLSS SR 权重 capture 跑机手册

目标：在 NVIDIA GPU Linux 机器上抓一次 DLSS SR first-frame 的 CUDA launch，
产出 MLX-DLSS `package-sr` 认的 capture 目录，换出 `dlss-sr.srmodel`。

背景：SR 权重在 `libnvidia-ngx-dlss.so` 里运行时组装，静态提取不可行
（MLX-DLSS 文档原话 "SR extraction directly from the library on a Mac is
not implemented"），必须一次运行时 capture。

## 机器要求

- NVIDIA GeForce GPU（RTX 3090/4090 最稳；Turing+）
- Linux x86_64 + 驱动 5xx + CUDA runtime（无需桌面，无头即可）
- gcc / python3 / curl
- 云实例（RunPod / Vast.ai 等）按小时租即可，抓一次全程 < 10 分钟

## 步骤

```bash
# 0) 把本目录 + libnvidia-ngx-dlss.so.310.7.0 拷到机器
scp -r capture/ user@gpu-box:~/sr/

# 1) 装配（拉 NGX 头文件 + 编译两个二进制）
cd ~/sr && chmod +x setup_capture_machine.sh && ./setup_capture_machine.sh

# 2) 抓取（LD_PRELOAD 挂钩 + 无头宿主跑 3 次 first-frame 评估）
mkdir cap
NVCUDA_CAPTURE_DIR=$PWD/cap \
  LD_PRELOAD=$PWD/nvcuda_capture.so \
  ./dlss_sr_host 2>&1 | tee host.log

#    若 [DLSSGate] 式权重未命中，按 preset 轮询（transformer = preset K，
#    枚举值逐个试）：
#    for p in $(seq 0 12); do
#      rm -rf cap && mkdir cap
#      NVCUDA_CAPTURE_DIR=$PWD/cap NVCUDA_HOST_PRESET=$p \
#        LD_PRELOAD=$PWD/nvcuda_capture.so ./dlss_sr_host && \
#        python3 triage_capture.py cap/ out_$p/ && break
#    done

# 3) 分拣（按 WEIGHTS/FILTER SHA 自动定位 launch 并重命名 L0001/L0012）
python3 triage_capture.py cap/ out/

# 4) 验证 + 转换（MLX-DLSS 仓库的 venv）
mlxdlss-weights package-sr libnvidia-ngx-dlss.so.310.7.0 out/ dlss-sr.srmodel
```

## 拿回 Mac 后

把 `out/`（capture）与 `dlss-sr.srmodel` 放进本仓库 `third_party/dlss/`，
通知 agent 跑 `package-sr` 验证 + 接入对比图集。

## 排障

| 症状 | 处置 |
|---|---|
| Init rc=0xBAD00001 FeatureNotSupported | ① 看 NVCUDA_HOST_DATA 目录下 NGX 日志（v3 宿主失败时会自动列出）；② 试 NVCUDA_HOST_APPID=1；③ 确认 GPU 是否 GeForce（数据中心卡 NGX 可能拒 DLSS） |
| host 报 MISSING 符号 | NGX 导出面与预期不符——把 host.log 发回，agent 调整符号表 |
| Init_Ext1 rc≠0 | 用普通 Init 路径（自动回退）；仍失败查 NGX 日志（AppDataPath 下） |
| triage 报权重未命中 | ① launch 数太少：加大 NVCUDA_CAPTURE_MAX；② NGX 走 VMM 分配：需扩 nvcuda_capture.c 的 cuMemCreate/cuMemMap 路径；③ preset 不对：按第 2 步轮询 |
| params 长度告警 | 参数块线性化走了 8 字节退化路径——说明驱动缺 cuFuncGetParamInfo（CUDA < 12.4），换新驱动 |

## Result 代码速查（nvsdk_ngx_defs.h）

| rc | 含义 |
|---|---|
| 0xBAD00001 | FeatureNotSupported——系统/硬件/图形 API 不支持该特性 |
| 0xBAD00002 | PlatformError——底层平台调用失败（查 NGX 日志） |
| 0xBAD00005 | InvalidParameter——参数缺失或类型错 |

## 已知边界

- v1 分配跟踪不含 VMM（cuMemCreate/cuMemMap）；cudart-static 的 dlsym 旁路未覆盖
- preset 枚举数值离线未知，机器上按 SHA 命中为准（triage 即裁判）
