#!/usr/bin/env bash
# 下载 sherpa-onnx 本地 ASR 模型
#
# 用法:
#   ./scripts/download-model.sh            # 默认下载流式在线模型
#   ./scripts/download-model.sh --offline  # 下载 X-ASR 非流式离线模型
#   ./scripts/download-model.sh --online   # 下载流式在线模型
set -euo pipefail

MODE="${1:---online}"

case "${MODE}" in
    --online)
        TARGET_DIR="${HOME}/.local/share/fcitx5/voice-input/models/zipformer"
        MODEL_NAME="sherpa-onnx-streaming-zipformer-zh-xlarge-int8-2025-06-30"
        MODEL_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/${MODEL_NAME}.tar.bz2"
        ;;
    --offline)
        TARGET_DIR="${HOME}/.local/share/fcitx5/voice-input/models/x-asr"
        MODEL_NAME="sherpa-onnx-x-asr-zipformer-transducer-zh-en-int8-2026-06-03"
        MODEL_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/${MODEL_NAME}.tar.bz2"
        ;;
    *)
        echo "未知参数: ${MODE}" >&2
        echo "用法: $0 [--online|--offline]" >&2
        exit 1
        ;;
esac

echo "=== 下载模型 (${MODEL_NAME}) ==="
echo "目标路径: ${TARGET_DIR}"

mkdir -p "${TARGET_DIR}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

echo "1. 正在从官方发布源下载模型压缩包..."
curl -SL --progress-bar "${MODEL_URL}" -o "${TMP_DIR}/model.tar.bz2"

echo "2. 正在解压到目标目录..."
tar -xjf "${TMP_DIR}/model.tar.bz2" -C "${TMP_DIR}"

# 清理目标目录下旧模型文件并移入新模型
rm -f "${TARGET_DIR}"/*.onnx "${TARGET_DIR}"/tokens.txt "${TARGET_DIR}"/bpe.model

SRC_DIR="${TMP_DIR}/${MODEL_NAME}"
cp -v "${SRC_DIR}"/encoder*.onnx "${TARGET_DIR}/"
cp -v "${SRC_DIR}"/decoder*.onnx "${TARGET_DIR}/"
cp -v "${SRC_DIR}"/joiner*.onnx "${TARGET_DIR}/"
cp -v "${SRC_DIR}"/tokens.txt "${TARGET_DIR}/"
if [ -f "${SRC_DIR}/bpe.model" ]; then
    cp -v "${SRC_DIR}"/bpe.model "${TARGET_DIR}/"
fi

echo "=== 模型准备完成！==="
ls -lh "${TARGET_DIR}"
