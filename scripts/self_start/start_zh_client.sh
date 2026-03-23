#!/bin/sh
BIN="/data/zh_work/zh_client"
SUP_PID_FILE="/var/run/zh_client_supervisor.pid"
SCRIPT_PATH="/data/zh_work/start_zh_client.sh"
RKAIQ_BIN="/oem/usr/bin/rkaiq_3A_server"
RKAIQ_FLAG_FILE="/var/run/rkaiq_3a_server.started"
LIB_DIR="/data/zh_work"
ONNX_LIB_REAL="$LIB_DIR/libonnxruntime.so.1.17.3"
BOOT_DELAY_SEC=3
SYS_LD_LIBRARY_PATH="/oem/usr/lib:/oem/lib:/usr/lib:/lib"
LOG_DIR="$LIB_DIR/logs"
DEBUG_LOG="$LOG_DIR/start_zh_client.log"

if [ -n "$LD_LIBRARY_PATH" ]; then
    RKAIQ_LD_LIBRARY_PATH="$SYS_LD_LIBRARY_PATH:$LD_LIBRARY_PATH"
    CLIENT_LD_LIBRARY_PATH="$LIB_DIR:$SYS_LD_LIBRARY_PATH:$LD_LIBRARY_PATH"
else
    RKAIQ_LD_LIBRARY_PATH="$SYS_LD_LIBRARY_PATH"
    CLIENT_LD_LIBRARY_PATH="$LIB_DIR:$SYS_LD_LIBRARY_PATH"
fi

[ -x "$BIN" ] || chmod 755 "$BIN"

ensure_log_dir() {
    [ -d "$LOG_DIR" ] || mkdir -p "$LOG_DIR" 2>/dev/null || true
}

log_debug() {
    ensure_log_dir
    echo "$(date '+%Y-%m-%d %H:%M:%S') [start_zh_client] $*" >>"$DEBUG_LOG"
}

is_running_pid() {
    [ -n "$1" ] && kill -0 "$1" 2>/dev/null
}

find_supervisor_pids() {
    ps | grep "$SCRIPT_PATH daemon" | grep -v grep | awk '{print $1}'
}

is_rkaiq_running() {
    ps | grep "$RKAIQ_BIN" | grep -v grep >/dev/null 2>&1
}

is_supervisor_running() {
    [ -f "$SUP_PID_FILE" ] || return 1
    OLD_PID="$(cat "$SUP_PID_FILE" 2>/dev/null)"
    is_running_pid "$OLD_PID" || return 1
    CMDLINE="$(cat /proc/$OLD_PID/cmdline 2>/dev/null | tr '\000' ' ')"
    echo "$CMDLINE" | grep -Fq "$SCRIPT_PATH daemon"
}

ensure_rkaiq_running() {
    if is_rkaiq_running; then
        log_debug "rkaiq already running"
        touch "$RKAIQ_FLAG_FILE"
        return 0
    fi
    while true; do
        log_debug "try start rkaiq_3A_server ld_library_path=$RKAIQ_LD_LIBRARY_PATH"
        ensure_log_dir
        LD_LIBRARY_PATH="$RKAIQ_LD_LIBRARY_PATH" "$RKAIQ_BIN" --silent >/dev/null 2>&1 &
        sleep 1
        if is_rkaiq_running; then
            log_debug "rkaiq started success"
            touch "$RKAIQ_FLAG_FILE"
            return 0
        fi
        log_debug "rkaiq not ready, retry after 1s"
        sleep 1
    done
}

prepare_runtime_libs() {
    if [ ! -f "$ONNX_LIB_REAL" ]; then
        return 1
    fi
    [ -e "$LIB_DIR/libonnxruntime.so.1" ] || ln -sf "$ONNX_LIB_REAL" "$LIB_DIR/libonnxruntime.so.1"
    [ -e "$LIB_DIR/libonnxruntime.so" ] || ln -sf "$ONNX_LIB_REAL" "$LIB_DIR/libonnxruntime.so"
    return 0
}

daemon_loop() {
    log_debug "daemon enter, boot delay=${BOOT_DELAY_SEC}s"
    sleep "$BOOT_DELAY_SEC"
    log_debug "daemon delay done"
    while true; do
        ensure_rkaiq_running
        if ! prepare_runtime_libs; then
            log_debug "onnxruntime missing, retry after 2s"
            sleep 2
            continue
        fi
        log_debug "launch zh_client ld_library_path=$CLIENT_LD_LIBRARY_PATH"
        ensure_log_dir
        LD_LIBRARY_PATH="$CLIENT_LD_LIBRARY_PATH" "$BIN" >/dev/null 2>&1
        log_debug "zh_client exited rc=$?, restart after 2s"
        sleep 2
    done
}

start_supervisor() {
    if is_supervisor_running; then
        log_debug "supervisor already running, skip"
        exit 0
    fi
    rm -f "$SUP_PID_FILE"
    ensure_log_dir
    setsid /bin/sh "$SCRIPT_PATH" daemon >>"$DEBUG_LOG" 2>&1 < /dev/null &
    echo $! >"$SUP_PID_FILE"
    log_debug "supervisor started pid=$!"
}

stop_supervisor() {
    for PID in $(find_supervisor_pids); do
        if is_running_pid "$PID"; then
            kill "$PID" 2>/dev/null || true
        fi
    done
    if [ -f "$SUP_PID_FILE" ]; then
        OLD_PID="$(cat "$SUP_PID_FILE" 2>/dev/null)"
        if is_running_pid "$OLD_PID"; then
            kill "$OLD_PID" 2>/dev/null || true
        fi
        rm -f "$SUP_PID_FILE"
    fi
    if command -v killall >/dev/null 2>&1; then
        killall zh_client 2>/dev/null || true
    else
        for PID in $(ps | grep "$BIN" | grep -v grep | awk '{print $1}'); do
            kill "$PID" 2>/dev/null || true
        done
    fi
}

case "$1" in
    daemon)
        log_debug "argv=daemon"
        daemon_loop
        ;;
    start|"")
        log_debug "argv=start"
        start_supervisor
        ;;
    stop)
        log_debug "argv=stop"
        stop_supervisor
        ;;
    restart)
        log_debug "argv=restart"
        stop_supervisor
        start_supervisor
        ;;
    *)
        echo "Usage: $0 {start|stop|restart}"
        exit 1
        ;;
esac

exit 0
