#!/bin/bash
# run_pydm.sh —— 启动 PyDM MSM 界面（负责补 Qt 平台插件依赖 + 注入前缀宏）
#
# 用法:
#   ./run_pydm.sh                 # 起界面
#   ./run_pydm.sh --regen         # 先从 pv_map.json 重新生成 msm_daq.ui
#   ./run_pydm.sh -- <pydm 参数>  # 追加参数透传
#
# 背景:
#   PyQt5 的 libqxcb.so 依赖 libxcb-xinerama.so.0，本机（Ubuntu 22.04）没装
#   且无 root。脚本按以下顺序解决，不需要 sudo：
#     1) 系统里已有           → 直接用
#     2) ~/.cache/msm/xcb 有  → 注入 LD_LIBRARY_PATH
#     3) 有网                  → apt-get download + dpkg-deb -x 到上面那个缓存目录
#     4) 都不行                → 退回 QT_QPA_PLATFORM=wayland（本机实测可用）

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
CFG="$ROOT/config/pre.cfg"
UI="$HERE/msm_daq.ui"
MAP="$ROOT/config/pv_map.json"
# pydm 位置：优先 $PYDM，其次 PATH（~/.local/bin/pydm），最后旧 venv 路径
# （/tmp 的 venv 会被系统清理，故不再作为首选）
PYDM="${PYDM:-$(command -v pydm 2>/dev/null || echo /tmp/opencode/pydm-venv/bin/pydm)}"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/msm/xcb"
LIBS="$CACHE/usr/lib/x86_64-linux-gnu"

die() { echo "[pydm] ERROR: $*" >&2; exit 1; }

[ -x "$PYDM" ] || die "找不到 pydm: $PYDM （用 PYDM=/path/to/pydm 指定；或 pip install pydm）"

# ── 可选：重新生成 .ui ──
if [ "${1:-}" = "--regen" ]; then
    python3 "$HERE/gen_pydm.py" -m "$MAP" -o "$UI" || die "gen_pydm.py 失败"
    shift
fi
[ -f "$UI" ] || die "缺 $UI （先跑 ./run_pydm.sh --regen）"

# ── 前缀：唯一真源 pre.cfg ──
[ -r "$CFG" ] || die "缺 $CFG"
set -a; # shellcheck disable=SC1090
source "$CFG"; set +a

# ── CA 地址 ──
# IOC 默认只监听 127.0.0.1（run_epics.sh 的 EPICS_CAS_INTF_ADDR_LIST），
# 而 CA 的广播发现不覆盖回环地址，所以客户端必须显式指定，否则 PV 全灰。
# 若 IOC 在别的机器上，用 MSM_IOC_ADDR=host[:port] 覆盖。
export EPICS_CA_ADDR_LIST="${MSM_IOC_ADDR:-127.0.0.1}"
export EPICS_CA_AUTO_ADDR_LIST=NO

# ── Qt 平台插件依赖 ──
XCB_PLUGIN="$("${PYDM%/*}/python" - <<'PY'
import os
try:
    import PyQt5
    print(os.path.join(os.path.dirname(PyQt5.__file__),
                       "Qt5", "plugins", "platforms", "libqxcb.so"))
except Exception:
    print("")
PY
)"

missing() { [ -n "$XCB_PLUGIN" ] && [ -f "$XCB_PLUGIN" ] && ldd "$XCB_PLUGIN" 2>/dev/null | grep -q "not found"; }

if [ -n "$XCB_PLUGIN" ] && [ -f "$XCB_PLUGIN" ]; then
    if ! missing; then
        :                                   # 系统依赖齐全
    elif [ -f "$LIBS/libxcb-xinerama.so.0" ]; then
        export LD_LIBRARY_PATH="$LIBS:${LD_LIBRARY_PATH:-}"
        echo "[pydm] 用缓存库: $LIBS"
    else
        echo "[pydm] libqxcb.so 依赖不全，尝试补 libxcb-xinerama0（无需 root）..."
        mkdir -p "$CACHE"
        if ( cd "$CACHE" && apt-get download libxcb-xinerama0 ) >/dev/null 2>&1 \
           && deb=$(ls "$CACHE"/libxcb-xinerama0_*.deb 2>/dev/null | head -1) \
           && [ -n "$deb" ] && dpkg-deb -x "$deb" "$CACHE" 2>/dev/null \
           && [ -f "$LIBS/libxcb-xinerama.so.0" ]; then
            export LD_LIBRARY_PATH="$LIBS:${LD_LIBRARY_PATH:-}"
            echo "[pydm] 已解包到 $CACHE"
        else
            echo "[pydm] 补库失败，退回 wayland 平台"
            export QT_QPA_PLATFORM=wayland
        fi
    fi
    if missing && [ "${QT_QPA_PLATFORM:-}" != "wayland" ]; then
        echo "[pydm] 仍然缺库，退回 wayland 平台"
        export QT_QPA_PLATFORM=wayland
    fi
else
    echo "[pydm] 找不到 libqxcb.so，使用默认平台"
fi

echo "[pydm] P='$P' I='$I'  platform=${QT_QPA_PLATFORM:-xcb(default)}"

# ── 宏：必须用 JSON 形式传递 ──
# pydm 的 parse_macro_string 对 KEY=value 形式有 bug：末尾键若为空值
# （如 "I="）不会被解析，导致 .ui 里 ${I} 残留、PV 名错误、界面全灰。
# JSON 是它的优先解析路径，空串能正确保留。
MACROS="$(python3 - "$P" "$I" <<'PY'
import json, sys
print(json.dumps({"P": sys.argv[1], "I": sys.argv[2]}))
PY
)"
echo "[pydm] macros: $MACROS"

PYBIN="${PYDM%/*}/python"
[ -x "$PYBIN" ] || PYBIN="$(command -v python3)"   # 系统 pydm（console script）无同目录 python
if [ -f "$HERE/msm_app.py" ] && [ -x "$PYBIN" ]; then
    # 带控制条的主程序：自己读 pre.cfg，动态注入宏
    exec "$PYBIN" "$HERE/msm_app.py" "$@"
fi
exec "$PYDM" --macro "$MACROS" "$UI" "$@"
