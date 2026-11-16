#!/bin/bash
# run_sim_stack.sh —— 一键起 "sim 后端 + IOC + IPC→CA 桥" 三段，供 PyDM 使用
#
# 用法:
#   ./run_sim_stack.sh start     # 起 backend(sim) + IOC + msm_epics，并自动 start 采集
#   ./run_sim_stack.sh stop      # 全部停掉
#   ./run_sim_stack.sh status    # 看进程 + 抽一个 PV 的值
#   ./run_sim_stack.sh restart
#
# 说明:
#   - PyDM 不在这里启动，自己开:  pydm --macro "P=...,I=..." main_epics/msm_daq.ui
#   - 与 main_webui 互斥使用（两边都抢 /tmp/msm_daq.sock，同一时刻只能有一个）
#   - backend 的 stdin 是控制通道：需要喂 "start" 才会真正出数，所以用 FIFO 常开

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
CFG="$ROOT/config/pre.cfg"
EPICS_BASE="${EPICS_BASE:-$ROOT/module_core/extern/epics}"
export EPICS_BASE
export LD_LIBRARY_PATH="$EPICS_BASE/lib/linux-x86_64:${LD_LIBRARY_PATH:-}"
export EPICS_CA_ADDR_LIST=127.0.0.1
export EPICS_CA_AUTO_ADDR_LIST=NO

BACKEND="$ROOT/main_cli/build/msm_cli_simsrc"
BRIDGE="$HERE/build/msm_epics"
IOC_BIN="$EPICS_BASE/bin/linux-x86_64/softIoc"
CAGET="$EPICS_BASE/bin/linux-x86_64/caget"

SOCK="/tmp/msm_daq.sock"
CTL="/tmp/msm_sim.cmd"

pidfile() { echo "/tmp/msm_$1.pid"; }

alive() {                       # alive <name>
    local f; f="$(pidfile "$1")"
    [ -f "$f" ] || return 1
    local p; p="$(cat "$f" 2>/dev/null)"
    [ -n "$p" ] && kill -0 "$p" 2>/dev/null
}

die() { echo "[sim_stack] ERROR: $*" >&2; exit 1; }

need() { [ -x "$1" ] || die "not executable: $1 (先编译)"; }

# ────────────────────────── start ──────────────────────────
do_start() {
    need "$BACKEND"; need "$BRIDGE"; need "$IOC_BIN"

    if alive backend; then
        echo "[sim_stack] backend 已在跑 (pid $(cat "$(pidfile backend)"))，跳过"
    else
        # 清场：残留 socket 会让新 backend bind 失败
        [ -S "$SOCK" ] && rm -f "$SOCK"

        [ -p "$CTL" ] || { rm -f "$CTL"; mkfifo "$CTL"; }

        # 0<> 以读写方式打开 FIFO：既拿到 stdin，又不会因写端关闭而 EOF
        nohup "$BACKEND" 0<> "$CTL" > /tmp/msm_simsrc.log 2>&1 &
        echo $! > "$(pidfile backend)"
        echo "[sim_stack] backend pid=$! log=/tmp/msm_simsrc.log"

        # 等 IPC socket 出现（backend 初始化 4 个 ring，约 1–3s）
        for _ in $(seq 1 40); do
            [ -S "$SOCK" ] && break
            sleep 0.25
        done
        [ -S "$SOCK" ] || die "backend 起来了但 $SOCK 没出现，看 /tmp/msm_simsrc.log"
        echo "[sim_stack] IPC ready: $SOCK"

        # 发 start，开始出数
        if alive backend; then
            echo "start" > "$CTL"
            echo "[sim_stack] sent 'start' to backend"
        else
            die "backend 已退出，看 /tmp/msm_simsrc.log"
        fi
    fi

    # IOC + 桥：交给 run_epics.sh（它负责前缀注入 / 生成 .db）
    if alive epics; then
        echo "[sim_stack] msm_epics 已在跑 (pid $(cat "$(pidfile epics)"))，跳过"
    else
        nohup bash "$HERE/run_epics.sh" > /tmp/msm_run_epics.log 2>&1 &
        echo $! > /tmp/msm_run_epics.pid
        echo "[sim_stack] run_epics pid=$! log=/tmp/msm_run_epics.log"
        sleep 6
    fi

    echo
    do_status
}

# ────────────────────────── stop ──────────────────────────
do_stop() {
    # 顺序：backend(正向) 先停会断数据源；先停桥和 IOC 更干净
    for n in epics ioc backend; do
        local f; f="$(pidfile "$n")"
        [ -f "$f" ] || continue
        local p; p="$(cat "$f" 2>/dev/null)"
        if [ -n "$p" ] && kill -0 "$p" 2>/dev/null; then
            # backend 走 graceful 更稳；不行再 KILL
            kill -TERM "$p" 2>/dev/null
            for _ in $(seq 1 20); do
                kill -0 "$p" 2>/dev/null || break
                sleep 0.1
            done
            kill -0 "$p" 2>/dev/null && kill -KILL "$p" 2>/dev/null
            echo "[sim_stack] stopped $n (pid=$p)"
        fi
        rm -f "$f"
    done

    # run_epics.sh 自己也会 fork，兜底按名字扫一遍（只杀这几个精确名字）
    for n in run_epics backend epics ioc; do
        local f2; f2="$(pidfile "$n")"
        [ "$n" = run_epics ] && f2="/tmp/msm_run_epics.pid"
        [ -f "$f2" ] || continue
        local p2; p2="$(cat "$f2" 2>/dev/null)"
        [ -n "$p2" ] && kill -KILL "$p2" 2>/dev/null
        rm -f "$f2"
    done
    for name in msm_epics softIoc msm_cli_simsrc; do
        for p in $(pgrep -x "$name" 2>/dev/null); do
            kill -KILL "$p" 2>/dev/null && echo "[sim_stack] killed stray $name (pid=$p)"
        done
    done

    rm -f "$SOCK" "$CTL"
    echo "[sim_stack] clean"
}

# ────────────────────────── status ──────────────────────────
do_status() {
    echo "[sim_stack] ---- processes ----"
    for n in backend epics ioc; do
        if alive "$n"; then
            echo "  $n   RUNNING  pid=$(cat "$(pidfile "$n")")"
        else
            echo "  $n   -"
        fi
    done
    echo "[sim_stack] socket: $([ -S "$SOCK" ] && echo ok || echo MISSING)"

    if [ -x "$CAGET" ] && [ -r "$CFG" ]; then
        # shellcheck disable=SC1090
        local P I
        set -a; source "$CFG"; set +a
        echo "[sim_stack] ---- sample PVs (P=$P I=$I) ----"
        for pv in "${P}${I}DAQ:ch1_frames" "${P}${I}Process:ch1_frames" \
                  "${P}${I}Storage:ch1_consumed_frames" "${P}${I}Process:x_pos"; do
            local v
            v="$("$CAGET" -t "$pv" 2>&1)"
            printf '  %-40s %s\n' "$pv" "${v:-<no response>}"
        done
        # 波形：NELM=4096，只看前 5 个元素确认非空
        local w
        w="$("$CAGET" -t "${P}${I}Process:wave_ch1" 2>&1 | tr -s ' ' | cut -d' ' -f1-5)"
        printf '  %-40s %s\n' "${P}${I}Process:wave_ch1[0:4]" "${w:-<no response>}"
    fi
}

# ────────────────────────── main ──────────────────────────
case "${1:-start}" in
    start)   do_start ;;
    stop)    do_stop ;;
    restart) do_stop; sleep 1; do_start ;;
    status)  do_status ;;
    *) echo "usage: $0 {start|stop|restart|status}"; exit 2 ;;
esac
