#!/bin/bash
# sr_train_setup.sh — SR 训练环境一键准备
# 用法: bash scripts/sr_train_setup.sh
set -e
echo "=== SR 训练环境检查 ==="

# 1. Python 虚拟环境
VENV="${HOME}/.venv"
if [ ! -d "$VENV" ]; then
  echo "创建虚拟环境 $VENV ..."
  python3 -m venv "$VENV"
fi
source "$VENV/bin/activate"

# 2. 依赖安装
echo "安装依赖 ..."
pip install --quiet torch torchvision numpy Pillow

# 3. DIV2K 数据下载（~3.5GB，仅需一次）
DATA_DIR="$HOME/sr_data"
mkdir -p "$DATA_DIR"
HR_DIR="$DATA_DIR/DIV2K_train_HR"

if [ ! -d "$HR_DIR" ] || [ $(ls "$HR_DIR"/*.png 2>/dev/null | wc -l) -lt 10 ]; then
  echo "下载 DIV2K train HR (~3.5GB) ..."
  ZIP="$DATA_DIR/DIV2K_train_HR.zip"

  # 尝试 HuggingFace 镜像
  URL_HF="https://huggingface.co/datasets/eugenesiow/Div2k/resolve/main/data/DIV2K_train_HR.zip"
  URL_ETH="http://data.vision.ee.ethz.ch/cvl/DIV2K/DIV2K_train_HR.zip"

  for URL in "$URL_HF" "$URL_ETH"; do
    echo "  尝试 $URL"
    curl -L --connect-timeout 15 -o "$ZIP" "$URL" 2>/dev/null && break
    rm -f "$ZIP"
  done

  if [ ! -f "$ZIP" ] || [ $(stat -f%z "$ZIP" 2>/dev/null || echo 0) -lt 1000000 ]; then
    echo "自动下载失败。请手动下载 DIV2K："
    echo "  https://data.vision.ee.ethz.ch/cvl/DIV2K/DIV2K_train_HR.zip"
    echo "  解压到 $HR_DIR/"
    exit 1
  fi

  echo "解压 ..."
  cd "$DATA_DIR" && unzip -q -o "$ZIP" && cd -
  echo "  $(ls "$HR_DIR" | wc -l | tr -d ' ') 张 HR 图"
else
  echo "DIV2K 已存在：$(ls "$HR_DIR"/*.png | wc -l | tr -d ' ') 张"
fi

echo ""
echo "=== 环境就绪 ==="
echo "启动训练："
echo "  python ContentTools/neural/sr_train_div2k.py --data_dir $DATA_DIR --epochs 100"
echo ""
echo "快速验证（5 epoch）:"
echo "  python ContentTools/neural/sr_train_div2k.py --data_dir $DATA_DIR --quick"
