#!/bin/sh

SUP_PID_FILE="/var/run/zh_client_supervisor.pid"
RKAIQ_FLAG_FILE="/var/run/rkaiq_3a_server.started"
DEBUG_LOG="/tmp/zh_startup_debug.log"
SCRIPT_PATH="/data/zh_work/start_zh_client.sh"

is_running_pid() {
    [ -n "$1" ] && kill -0 "$1" 2>/dev/null
}

# Stop supervisor loop first to avoid auto-restart.
for PID in $(ps | grep "$SCRIPT_PATH daemon" | grep -v grep | awk '{print $1}'); do
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
    killall rkaiq_3A_server 2>/dev/null || true
    killall face_engine 2>/dev/null || true
else
    for PID in $(ps | grep "/data/zh_work/zh_client" | grep -v grep | awk '{print $1}'); do
        kill "$PID" 2>/dev/null || true
    done
    for PID in $(ps | grep "/oem/usr/bin/rkaiq_3A_server" | grep -v grep | awk '{print $1}'); do
        kill "$PID" 2>/dev/null || true
    done
    for PID in $(ps | grep "/data/zh_work/face/face_engine" | grep -v grep | awk '{print $1}'); do
        kill "$PID" 2>/dev/null || true
    done
fi

# Final sweep: force-kill any leftovers.
for PID in $(ps | grep -E "/data/zh_work/zh_client|/oem/usr/bin/rkaiq_3A_server|/data/zh_work/face/face_engine" | grep -v grep | awk '{print $1}'); do
    kill -9 "$PID" 2>/dev/null || true
done

rm -f "$RKAIQ_FLAG_FILE"
echo "$(date '+%Y-%m-%d %H:%M:%S') [kill.sh] all related processes stopped" >>"$DEBUG_LOG"

exit 0
