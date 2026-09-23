#!/bin/bash
# setup_capture_machine.sh — 在 NVIDIA GPU Linux 机器上装配 SR capture 环境。
# 前提：NVIDIA 驱动已装（nvidia-smi 可用）；无需桌面环境。
# 注意：dlss_sr_host.cpp 用 dlsym 绑 NGX 符号，编译不依赖 NGX 头文件；
# （宿主已 dlsym 化，无需 NGX 头）。
set -uo pipefail   # 不用 -e：头文件下载失败不中断构建

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

echo "== 1. 环境自检 =="
command -v nvidia-smi >/dev/null || { echo "缺 nvidia-smi（先装 NVIDIA 驱动）" >&2; exit 1; }
nvidia-smi | head -12
gcc --version | head -1 || { echo "缺 gcc（apt install build-essential）" >&2; exit 1; }

echo "== 2. 编译（不依赖 NGX 头文件） =="
gcc -O2 -shared -fPIC -o nvcuda_capture.so nvcuda_capture.c -ldl -lpthread || {
  echo "nvcuda_capture.so 编译失败" >&2; exit 1; }
g++ -O2 -o dlss_sr_host dlss_sr_host.cpp -ldl || {
  echo "dlss_sr_host 编译失败" >&2; exit 1; }
ls -la nvcuda_capture.so dlss_sr_host

echo "== 3. 拉取 NGX SDK 头文件（仅参考；失败可忽略） =="
mkdir -p ngx_include
BASE="https://raw.githubusercontent.com/NVIDIA/DLSS/main/include"
for h in nvsdk_ngx.h nvsdk_ngx_defs.h nvsdk_ngx_params.h nvsdk_ngx_loader.h \
         nvsdk_ngx_standalone_common.h nvsdk_ngx_standalone_cuda.h \
         nvsdk_ngx_helpers.h nvsdk_ngx_helpers_cuda.h; do
  if [ ! -s "ngx_include/$h" ]; then
    curl -fsSL --connect-timeout 10 "$BASE/$h" -o "ngx_include/$h" \
      || echo "  跳过 $h（下载失败，不影响使用）"
  fi
done
ls ngx_include/ 2>/dev/null || true

echo "== 4. NGX 库就位 =="
[ -f libnvidia-ngx-dlss.so.310.7.0 ] || {
  echo "把 libnvidia-ngx-dlss.so.310.7.0 放到 $HERE/" >&2; exit 1; }

echo "== 完成。跑法："
echo "  mkdir cap"
echo "  NVCUDA_CAPTURE_DIR=\$PWD/cap LD_PRELOAD=\$PWD/nvcuda_capture.so ./dlss_sr_host"
echo "  python3 triage_capture.py cap/ out/"
