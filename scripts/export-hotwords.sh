#!/usr/bin/env bash
set -euo pipefail

OUTPUT_FILE="${1:-${HOME}/.local/share/fcitx5/voice-input/hotwords.txt}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

mkdir -p "$(dirname "${OUTPUT_FILE}")" 2>/dev/null || true
RAW_WORDS="${TMP_DIR}/raw_words.txt"
touch "${RAW_WORDS}"

echo "=== 从高质量专业词库中提炼专属纯净热词 ==="

# 1. 导出用户自定义短语 (~/.config/fcitx5/conf/customphrase，最干净、最高优先级)
CUSTOM_PHRASE="${HOME}/.config/fcitx5/conf/customphrase"
if [ -f "${CUSTOM_PHRASE}" ]; then
    echo "• 正在解析用户自定义短语: ${CUSTOM_PHRASE}..."
    awk '{if (NF>=2) print $2; else print $1}' "${CUSTOM_PHRASE}" >> "${RAW_WORDS}"
fi

# 2. 导出专业技术词库 (~/.local/share/fcitx5/pinyin/dictionaries/*.dict)
DICT_DIR="${HOME}/.local/share/fcitx5/pinyin/dictionaries"
if [ -d "${DICT_DIR}" ] && command -v libime_pinyindict &>/dev/null; then
    for dict in "${DICT_DIR}"/*.dict; do
        if [ -f "${dict}" ]; then
            dict_name="$(basename "${dict}")"
            echo "• 正在解析专业技术词库: ${dict_name}..."
            libime_pinyindict -d "${dict}" "${TMP_DIR}/ext_${dict_name}.txt" 2>/dev/null || true
            if [ -f "${TMP_DIR}/ext_${dict_name}.txt" ]; then
                awk '{print $1}' "${TMP_DIR}/ext_${dict_name}.txt" >> "${RAW_WORDS}"
            fi
        fi
    done
fi

echo "• 正在进行高精度专业词过滤（剔除日常白话、错词、过滤无意义字）..."

python3 - <<EOF
import re
from collections import Counter

raw_path = "${RAW_WORDS}"
out_path = "${OUTPUT_FILE}"

# 过滤掉不需要热词加分的日常通用高频白话（模型原本就能 100% 听懂，加了反而干扰概率分布）
COMMON_STOPWORDS = {
    "这个", "那个", "怎么", "可以", "什么", "没有", "应该", "现在", "继续", "当前",
    "进行", "实现", "问题", "如果", "但是", "因为", "所以", "然后", "虽然", "我们",
    "他们", "你们", "自己", "已经", "还是", "或者", "为了", "比较", "非常", "十分",
    "一下", "一些", "一次", "一直", "一定", "一起", "一样", "一般", "一点", "主要"
}

counter = Counter()

with open(raw_path, "r", encoding="utf-8", errors="ignore") as f:
    for line in f:
        word = line.strip()
        if not word:
            continue
        
        # 长度规则：只保留 2 到 6 个汉字，或 2 到 15 个字母的专有名词
        if len(word) < 2 or len(word) > 15:
            continue
            
        # 仅保留合法字词
        if not re.match(r'^[\u4e00-\u9fa5a-zA-Z0-9_\+\-\#\.]+$', word):
            continue

        if word in COMMON_STOPWORDS:
            continue

        counter[word] += 1

# 提取最精炼的高质量专业热词（控制在 500 个以内，确保 Context Graph 高度灵敏且无误触）
top_words = [word for word, count in counter.most_common(500)]

with open(out_path, "w", encoding="utf-8") as f:
    for w in top_words:
        f.write(w + "\n")

print(f"✓ 成功生成 {len(top_words)} 个纯净专业热词！")
EOF

echo "=== 精品热词已保存至: ${OUTPUT_FILE} ==="
echo "精选词汇预览："
head -n 25 "${OUTPUT_FILE}"
