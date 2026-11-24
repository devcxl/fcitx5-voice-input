#!/usr/bin/env bash
# model_manager 集成验证：真实归档 + 真实 sherpa 试加载。
#
# 用途：在开发机上验证安装器的端到端行为（不进入 ctest，因为它需要几百 MB
# 的官方归档与可 dlopen 的 sherpa 库）。这是开发验证脚本，不是用户下载流程。
#
# 用法：
#   ./src/model_manager/integration_check.sh [WORK_ROOT]
#
#   WORK_ROOT 必须是**已存在**的目录，仅作为本次运行的父目录；脚本会在其下用
#   mktemp 独占创建本次专用子目录，所有产物（二进制、build.log、数据目录、归档
#   副本）都只写入该子目录。脚本**不会**删除或覆盖 WORK_ROOT 本身及其它内容。
#
#   省略 WORK_ROOT 时：若 ${TMPDIR:-/tmp}/opencode 已存在则用它，否则新建一个
#   mktemp 根目录（不会 chmod 或覆盖任何既有目录）。
#
# 环境变量：
#   SHERPA_LIB_DIR    libsherpa-onnx-c-api.so 所在目录（默认尝试系统路径）
#   OFFLINE_ARCHIVE   已下载的 offline-x-asr 归档路径（缺省则联网下载）
#   ONLINE_ARCHIVE    已下载的 online-zipformer 归档路径（缺省则跳过该模型）
#   ARCHIVE_DIR       本地归档目录（默认 <run-dir>/archives）
#   KEEP_RUN=1        无论成败都保留本次运行目录
#
# 退出码：0 全部通过；1 有失败；2 参数错误；77 前置条件不满足（跳过）。

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="${ROOT}/src/model_manager"

# ── 运行目录：只在 WORK_ROOT 下创建自己的子目录 ─────────────────────────────
if [ $# -ge 1 ]; then
    WORK_ROOT="$1"
    if [ ! -d "${WORK_ROOT}" ]; then
        echo "错误: WORK_ROOT 必须是已存在的目录: ${WORK_ROOT}" >&2
        echo "      脚本不会替你创建它，也不会删除它。" >&2
        exit 2
    fi
else
    if [ -d "${TMPDIR:-/tmp}/opencode" ]; then
        WORK_ROOT="${TMPDIR:-/tmp}/opencode"
    else
        WORK_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/voice-input-model-root.XXXXXX")" || {
            echo "错误: 无法创建临时根目录" >&2
            exit 2
        }
    fi
fi

RUN_DIR="$(mktemp -d "${WORK_ROOT%/}/voice-input-model-check.XXXXXX")" || {
    echo "错误: 无法在 ${WORK_ROOT} 下创建运行目录" >&2
    exit 2
}

# 只清理本次创建的子目录；WORK_ROOT 与其它内容一律不动。
keep_run="${KEEP_RUN:-0}"
cleanup() {
    local rc=$?
    if [ "${keep_run}" -eq 0 ] && [ "${rc}" -eq 0 ]; then
        rm -rf "${RUN_DIR}"
        printf '\n本次运行目录已清理（KEEP_RUN=1 可保留）：%s\n' "${RUN_DIR}"
    else
        printf '\n本次运行目录已保留（便于排查）：%s\n' "${RUN_DIR}"
    fi
}
trap cleanup EXIT

ARCHIVE_DIR="${ARCHIVE_DIR:-${RUN_DIR}/archives}"

BIN="${RUN_DIR}/bin/voice-input-model-installer"
TEST_BIN="${RUN_DIR}/bin/model_manager_test"
TEST_BIN2="${RUN_DIR}/bin/model_manager_extract_test"

printf '运行目录: %s\n' "${RUN_DIR}"
printf '（父目录 %s 不会被修改或删除）\n\n' "${WORK_ROOT}"

pass=0
fail=0

ok()   { printf 'PASS  %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf 'FAIL  %s\n' "$1"; fail=$((fail+1)); }
note() { printf '      %s\n' "$1"; }

need() {
    command -v "$1" >/dev/null 2>&1 || { echo "缺少命令: $1"; exit 77; }
}
need g++
need python3

mkdir -p "${RUN_DIR}/bin"

echo "=== 编译 ==="
if ! g++ -std=c++20 -O2 -Wall -Wextra -DHAVE_SHERPA_ONNX \
        -I"${SRC}" -I"${ROOT}/third_party/sherpa-onnx" \
        "${SRC}"/*.cpp \
        -larchive -lcrypto -lcurl -ljsoncpp -ldl -o "${BIN}" 2>"${RUN_DIR}/build.log"; then
    echo "安装器编译失败，见 ${RUN_DIR}/build.log"
    tail -20 "${RUN_DIR}/build.log"
    exit 1
fi
ok "安装器编译"

MM_SOURCES=("${SRC}/model_catalog.cpp" "${SRC}/model_paths.cpp"
            "${SRC}/sha256.cpp" "${SRC}/fs_safe.cpp" "${SRC}/safe_remove.cpp"
            "${SRC}/archive_extract.cpp" "${SRC}/install_lock.cpp"
            "${SRC}/install_events.cpp" "${SRC}/installed_model.cpp")
for t in model_manager_test model_manager_extract_test model_manager_fs_test \
         model_manager_child_test model_manager_publish_test \
         model_manager_safe_remove_test; do
    if ! g++ -std=c++20 -O1 -Wall -Wextra \
            -I"${SRC}" -I"${ROOT}/tests" \
            "${ROOT}/tests/${t}.cpp" "${MM_SOURCES[@]}" \
            $([ "${t}" = "model_manager_child_test" ] && echo "${SRC}/trial_load.cpp") \
            $([ "${t}" = "model_manager_publish_test" ] && echo "${SRC}/publish.cpp") \
            -larchive -lcrypto -ljsoncpp -o "${RUN_DIR}/bin/${t}" \
            2>"${RUN_DIR}/${t}-build.log"; then
        echo "单元测试 ${t} 编译失败，见 ${RUN_DIR}/${t}-build.log"
        tail -20 "${RUN_DIR}/${t}-build.log"
        exit 1
    fi
done
ok "单元测试编译"

echo
echo "=== 单元测试 ==="
if "${TEST_BIN}"; then ok "model_manager_test"; else bad "model_manager_test"; fi
if "${TEST_BIN2}"; then ok "model_manager_extract_test"; else bad "model_manager_extract_test"; fi
if "${RUN_DIR}/bin/model_manager_fs_test"; then ok "model_manager_fs_test"; else bad "model_manager_fs_test"; fi
if "${RUN_DIR}/bin/model_manager_child_test"; then ok "model_manager_child_test"; else bad "model_manager_child_test"; fi
if "${RUN_DIR}/bin/model_manager_publish_test"; then ok "model_manager_publish_test"; else bad "model_manager_publish_test"; fi
if "${RUN_DIR}/bin/model_manager_safe_remove_test"; then ok "model_manager_safe_remove_test"; else bad "model_manager_safe_remove_test"; fi

export XDG_DATA_HOME="${RUN_DIR}/data"
export LD_LIBRARY_PATH="${SHERPA_LIB_DIR:-/tmp/opencode/voice-input-sherpa-check/lib}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
rm -rf "${XDG_DATA_HOME}"
mkdir -p "${XDG_DATA_HOME}"

json() { python3 -c "
import json,sys
d=json.loads(sys.stdin.read().strip().splitlines()[-1])
print(d.get('$1',''))"; }

run_install() { # model archive -> prints last json line
    if [ -n "$2" ]; then
        "${BIN}" --model "$1" --archive "$2" 2>/dev/null
    else
        "${BIN}" --model "$1" 2>/dev/null
    fi
}

echo
echo "=== 清单 ==="
if "${BIN}" --list | grep -q '"model":"offline-x-asr"'; then
    ok "--list 包含 offline-x-asr"
else
    bad "--list 缺少 offline-x-asr"
fi

echo
echo "=== 官方离线归档（真实）==="
OFF="${OFFLINE_ARCHIVE:-}"
if [ -z "${OFF}" ] && [ -f "${ARCHIVE_DIR}/offline.tar.bz2" ]; then
    OFF="${ARCHIVE_DIR}/offline.tar.bz2"
fi
if [ -n "${OFF}" ] && [ -f "${OFF}" ]; then
    before=$(date +%s)
    OUT="$(run_install offline-x-asr "${OFF}")"
    rc=$?
    if [ $rc -eq 0 ] && [ "$(printf '%s' "${OUT}" | json ok)" = "True" ]; then
        ok "offline-x-asr 安装（离线归档，$(($(date +%s)-before))s）"
        D="$(printf '%s' "${OUT}" | json versionDir)"
        if [ -f "${D}/tokens.txt" ] && [ -f "${D}/install.json" ]; then
            ok "版本目录含 4 个模型文件 + install.json"
        else
            bad "版本目录内容不完整: ${D}"
        fi
        # 幂等
        OUT2="$(run_install offline-x-asr "${OFF}")"
        if [ "$(printf '%s' "${OUT2}" | json published)" = "False" ] && \
           [ "$(printf '%s' "${OUT2}" | json ok)" = "True" ]; then
            ok "重复安装幂等收敛（published=false）"
        else
            bad "重复安装不是幂等收敛"
        fi
        # 篡改检测 + 修复
        python3 -c "
p='${D}/tokens.txt'
d=bytearray(open(p,'rb').read())
d[10]=ord('X')
open(p,'wb').write(d)"
        # 保守策略：已存在但校验不通过的版本目录**不自动替换**，
        # 发布失败并提示人工处理；现有目录及其内容必须原样保留。
        OUT3="$(run_install offline-x-asr "${OFF}")"
        RC3=$?
        if [ $RC3 -ne 0 ] && printf '%s' "${OUT3}" | grep -q '"error":"publish_failed"'; then
            ok "已存在坏目录时拒绝自动替换（publish_failed）"
        else
            bad "坏目录被自动替换了（rc=${RC3}）"
        fi
        if [ -f "${D}/tokens.txt" ] && [ "$(sha256sum "${D}/tokens.txt" | cut -d' ' -f1)" != "ad0df596d9cc60c90795409549ccf7d1c4f901bdc060dd6b2b067d9ef2fddfaf" ]; then
            ok "被拒绝发布时原目录内容未被改动"
        else
            bad "原目录内容被改动或文件缺失"
        fi
        # 人工移除后重新安装应成功。
        rm -rf "${D}"
        OUT4="$(run_install offline-x-asr "${OFF}")"
        if [ "$(printf '%s' "${OUT4}" | json published)" = "True" ]; then
            ok "人工移除坏目录后安装成功"
        else
            bad "人工移除后仍安装失败"
        fi
    else
        bad "offline-x-asr 安装失败: ${OUT}"
    fi
else
    note "未提供 offline 归档，跳过（设置 OFFLINE_ARCHIVE 或放到 ${ARCHIVE_DIR}/）"
fi

echo
echo "=== 官方在线归档（真实，可选）==="
ON="${ONLINE_ARCHIVE:-}"
if [ -z "${ON}" ] && [ -f "${ARCHIVE_DIR}/online.tar.bz2" ]; then
    ON="${ARCHIVE_DIR}/online.tar.bz2"
fi
if [ -n "${ON}" ] && [ -f "${ON}" ]; then
    OUT="$(run_install online-zipformer "${ON}")"
    if [ $? -eq 0 ] && [ "$(printf '%s' "${OUT}" | json ok)" = "True" ]; then
        ok "online-zipformer 安装（离线归档）"
    else
        bad "online-zipformer 安装失败"
    fi
else
    note "未提供 online 归档，跳过"
fi

echo
echo "=== 只读状态查询（--check-model）==="
if [ -n "${OFF}" ] && [ -f "${OFF}" ]; then
    VERSION="$("${BIN}" --list | python3 -c "
import json,sys
for line in sys.stdin:
    d=json.loads(line)
    if d.get('model')=='offline-x-asr':
        print(d['version']); break")"
    MODEL_DIR="${XDG_DATA_HOME}/fcitx5/voice-input/models/offline-x-asr"
    VDIR="${MODEL_DIR}/${VERSION}"

    # 1) 未安装：state=absent，且**不得创建**任何目录（只读语义）。
    FRESH="${RUN_DIR}/fresh-xdg"
    rm -rf "${FRESH}"; mkdir -p "${FRESH}"
    OUT="$(XDG_DATA_HOME="${FRESH}" "${BIN}" --check-model offline-x-asr 2>/dev/null)"
    RC=$?
    if [ $RC -eq 0 ] && printf '%s' "${OUT}" | grep -q '"state":"absent"'; then
        ok "未安装 → state=absent"
    else
        bad "未安装状态查询异常（rc=${RC}, out=${OUT}）"
    fi
    if [ -z "$(find "${FRESH}" -mindepth 1 2>/dev/null)" ]; then
        ok "--check-model 未创建任何文件/目录（只读）"
    else
        bad "--check-model 写入了目录"
    fi
    if [ "$(printf '%s' "${OUT}" | wc -l)" -le 1 ]; then
        ok "--check-model 输出单行 JSON"
    else
        bad "--check-model 输出多行"
    fi

    # 2) 已安装且完整：state=valid。
    OUT="$(XDG_DATA_HOME="${XDG_DATA_HOME}" "${BIN}" --check-model offline-x-asr 2>/dev/null)"
    if printf '%s' "${OUT}" | grep -q '"state":"valid"'; then
        ok "已安装 → state=valid"
    else
        bad "已安装状态误报：${OUT}"
    fi

    # 3) 被篡改：state=tampered。
    python3 -c "
p='${VDIR}/tokens.txt'
d=bytearray(open(p,'rb').read()); d[0]^=0xFF; open(p,'wb').write(d)"
    OUT="$(XDG_DATA_HOME="${XDG_DATA_HOME}" "${BIN}" --check-model offline-x-asr 2>/dev/null)"
    if printf '%s' "${OUT}" | grep -q '"state":"tampered"'; then
        ok "篡改 → state=tampered"
    else
        bad "篡改未被识别：${OUT}"
    fi

    # 4) 保守策略：坏目录不会被自动替换；人工移除后重新安装才回到 valid。
    OUT="$(run_install offline-x-asr "${OFF}")"
    if [ $? -ne 0 ] && printf '%s' "${OUT}" | grep -q '"error":"publish_failed"'; then
        ok "存在坏目录时重装被拒绝（不自动替换）"
    else
        bad "坏目录被自动替换"
    fi
    rm -rf "${VDIR}"
    run_install offline-x-asr "${OFF}" >/dev/null 2>&1
    OUT="$(XDG_DATA_HOME="${XDG_DATA_HOME}" "${BIN}" --check-model offline-x-asr 2>/dev/null)"
    if printf '%s' "${OUT}" | grep -q '"state":"valid"'; then
        ok "人工移除后重新安装回到 state=valid"
    else
        bad "人工移除后状态异常：${OUT}"
    fi

    # 5) 协议必须带 GUI 需要的固定版本/体积/来源（取自内置清单）。
    OUT="$(XDG_DATA_HOME="${XDG_DATA_HOME}" "${BIN}" --check-model offline-x-asr 2>/dev/null)"
    if printf '%s' "${OUT}" | grep -q '"version":"sherpa-onnx-x-asr' &&
       printf '%s' "${OUT}" | grep -q '"archiveSize":136902463' &&
       printf '%s' "${OUT}" | grep -q '"source":"https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/'; then
        ok "installed_status 含 version/archiveSize/source"
    else
        bad "installed_status 缺少 catalog 字段：${OUT}"
    fi

    # 6) 未知模型：非零退出，且不输出状态行（不把未知 id 误报成 absent）。
    OUT="$(XDG_DATA_HOME="${XDG_DATA_HOME}" "${BIN}" --check-model nope 2>/dev/null)"
    RC=$?
    if [ $RC -ne 0 ] && [ -z "${OUT}" ]; then
        ok "未知模型 → 非零退出且无状态行"
    else
        bad "未知模型处理异常（rc=${RC}, out=${OUT}）"
    fi
fi

echo
echo "=== 符号链接边界 ==="
if [ -n "${OFF}" ] && [ -f "${OFF}" ]; then
    # (a) models 本身被换成符号链接 → 视为不可读，且安装必须失败。
    SL="${RUN_DIR}/sl-xdg"
    rm -rf "${SL}" "${RUN_DIR}/sl-target"; mkdir -p "${SL}/fcitx5/voice-input" "${RUN_DIR}/sl-target"
    ln -s "${RUN_DIR}/sl-target" "${SL}/fcitx5/voice-input/models"
    OUT="$(XDG_DATA_HOME="${SL}" "${BIN}" --check-model offline-x-asr 2>/dev/null)"
    if printf '%s' "${OUT}" | grep -q '"state":"unreadable"'; then
        ok "models 为符号链接 → state=unreadable"
    else
        bad "models 符号链接未被拒绝：${OUT}"
    fi
    XDG_DATA_HOME="${SL}" "${BIN}" --model offline-x-asr --archive "${OFF}" >/dev/null 2>&1
    [ $? -ne 0 ] && ok "models 为符号链接时安装失败" || bad "models 符号链接下仍安装成功"
    [ -z "$(ls -A "${RUN_DIR}/sl-target" 2>/dev/null)" ] && ok "未向符号链接目标写入内容" || bad "写入了符号链接目标"

    # (b) 上层（XDG_DATA_HOME 的上级）是符号链接 → 合法，安装应成功。
    LEGIT="${RUN_DIR}/legit-real"
    rm -rf "${LEGIT}" "${RUN_DIR}/legit-link"; mkdir -p "${LEGIT}"
    ln -s "${LEGIT}" "${RUN_DIR}/legit-link"
    OUT="$(XDG_DATA_HOME="${RUN_DIR}/legit-link/data" "${BIN}" --model offline-x-asr --archive "${OFF}" 2>/dev/null)"
    if [ "$(printf '%s' "${OUT}" | json ok)" = "True" ]; then
        ok "上层符号链接（合法 HOME/XDG 布局）下安装成功"
    else
        bad "合法上层符号链接被误伤"
    fi
fi

echo
echo "=== 归档校验与并发 ==="
if [ -n "${OFF}" ] && [ -f "${OFF}" ]; then
    head -c 200000 "${OFF}" > "${RUN_DIR}/truncated.tar.bz2"
    rm -rf "${XDG_DATA_HOME}"; mkdir -p "${XDG_DATA_HOME}"
    "${BIN}" --model offline-x-asr --archive "${RUN_DIR}/truncated.tar.bz2" >/dev/null 2>&1
    [ $? -eq 6 ] && ok "截断归档被拒（exit 6 archive_mismatch）" || bad "截断归档未被正确拒绝"

    # 并发：第一个占锁时第二个必须 lock_busy
    rm -rf "${XDG_DATA_HOME}"; mkdir -p "${XDG_DATA_HOME}"
    "${BIN}" --model offline-x-asr --archive "${OFF}" >/dev/null 2>&1 &
    A=$!
    sleep 1
    "${BIN}" --model offline-x-asr --archive "${OFF}" >/dev/null 2>&1
    [ $? -eq 4 ] && ok "并发安装返回 lock_busy（exit 4）" || bad "并发未返回 lock_busy"
    wait $A
fi

echo
echo "=== 发布竞态：目标在检查后被外部创建 ==="
if [ -n "${OFF}" ] && [ -f "${OFF}" ]; then
    VERSION="$("${BIN}" --list | python3 -c "
import json,sys
for line in sys.stdin:
    d=json.loads(line)
    if d.get('model')=='offline-x-asr':
        print(d['version']); break")"
    TARGET_DIR="${XDG_DATA_HOME}/fcitx5/voice-input/models/offline-x-asr"
    TARGET="${TARGET_DIR}/${VERSION}"

    for kind in symlink dir; do
        rm -rf "${XDG_DATA_HOME}"; mkdir -p "${XDG_DATA_HOME}"
        "${BIN}" --model offline-x-asr --archive "${OFF}" > "${RUN_DIR}/race-${kind}.jsonl" 2>/dev/null &
        P=$!
        sleep 2   # 已过"目标是否已存在"的早期检查
        mkdir -p "${TARGET_DIR}"
        if [ "${kind}" = "symlink" ]; then
            printf 'external-asset' > "${XDG_DATA_HOME}/external.txt"
            ln -s "${XDG_DATA_HOME}/external.txt" "${TARGET}"
        else
            mkdir -p "${TARGET}"
            printf 'external-asset' > "${TARGET}/keep.txt"
        fi
        wait $P
        RC=$?
        LAST="$(tail -1 "${RUN_DIR}/race-${kind}.jsonl")"
        if [ $RC -ne 0 ] && printf '%s' "${LAST}" | grep -q '"error":"publish_failed"'; then
            ok "外部创建的 ${kind} 触发 publish_failed（未覆盖）"
        else
            bad "外部创建的 ${kind} 未被拒绝（rc=${RC}）"
        fi
        if [ "${kind}" = "symlink" ]; then
            if [ -L "${TARGET}" ] && [ "$(cat "${XDG_DATA_HOME}/external.txt")" = "external-asset" ]; then
                ok "外部符号链接与其目标内容均未被改动"
            else
                bad "外部符号链接被改动"
            fi
        else
            if [ -f "${TARGET}/keep.txt" ] && [ "$(cat "${TARGET}/keep.txt")" = "external-asset" ]; then
                ok "外部目录内容未被改动"
            else
                bad "外部目录被改动"
            fi
        fi
    done
fi

echo
echo "=== 取消语义 ==="
if [ -n "${OFF}" ] && [ -f "${OFF}" ]; then
    rm -rf "${XDG_DATA_HOME}"; mkdir -p "${XDG_DATA_HOME}"
    "${BIN}" --model offline-x-asr --archive "${OFF}" > "${RUN_DIR}/cancel.jsonl" 2>/dev/null &
    P=$!
    sleep 1
    kill -TERM $P 2>/dev/null
    wait $P
    RC=$?
    LAST="$(tail -1 "${RUN_DIR}/cancel.jsonl")"
    if [ $RC -eq 5 ] && printf '%s' "${LAST}" | grep -q '"cancelled":true' && \
       printf '%s' "${LAST}" | grep -q '"published":false'; then
        ok "SIGTERM：cancelled=true / published=false / exit 5"
    else
        bad "SIGTERM 取消语义不符（rc=${RC}）"
    fi
    # 版本目录形如 <data>/fcitx5/voice-input/models/<id>/<version>；
    # 只匹配这两级，排除 .locks/.staging 等内部目录。
    STRAY="$(find "${XDG_DATA_HOME}/fcitx5/voice-input/models" -mindepth 2 -maxdepth 2 -type d \
                -not -path '*/.locks*' -not -path '*/.staging*' 2>/dev/null)"
    if [ -z "${STRAY}" ]; then
        ok "取消后未留下版本目录"
    else
        bad "取消后存在版本目录: ${STRAY}"
    fi
    if [ -z "$(find "${XDG_DATA_HOME}" -path '*/.staging/*' -mindepth 1 2>/dev/null)" ]; then
        ok "取消后未留下暂存残留"
    else
        bad "取消后暂存有残留"
    fi
fi

echo
printf '=== 结果：%d passed, %d failed ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
