#!/bin/sh
set -eu

: "${PS5_HOST:?set PS5_HOST to the FW 5.50 console address}"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(dirname -- "$script_dir")
build_dir=${SDL_PS5AGC_BUILD_DIR:-$repo_dir/build-ps5agc-native2}
elf=${SDL_PS5AGC_PROBE_ELF:-$build_dir/test/testyuv}
bmp=${SDL_PS5AGC_PROBE_BMP:-$build_dir/test/testyuv.bmp}
websrv_timeout=${SDL_PS5AGC_WEBSRV_TIMEOUT:-30}
probe_frames=${SDL_PS5AGC_PROBE_FRAMES:-1}
probe_renderer=${SDL_PS5AGC_PROBE_RENDERER:-ps5agc}
klog_port=${SDL_PS5AGC_KLOG_PORT:-3232}
pyps4debug_dir=${PYPS4DEBUG_DIR:-/Users/bizkut/Downloads/PS5/homebrew/PyPS4debug}
killer=${PS5DEBUG_KILLER:-$repo_dir/../Vulkan-PS5/examples/ps5debug_kill_process.py}
log_dir=${SDL_PS5AGC_LOG_DIR:-${TMPDIR:-/tmp}/sdl-ps5agc-qualification}
remote_dir=/data/homebrew/sdl_ps5agc_display_probe

case "$websrv_timeout" in
    ''|*[!0-9]*|0)
        echo "SDL_PS5AGC_WEBSRV_TIMEOUT must be a positive integer" >&2
        exit 2
        ;;
esac
case "$probe_frames" in
    ''|*[!0-9]*|0)
        echo "SDL_PS5AGC_PROBE_FRAMES must be a positive integer" >&2
        exit 2
        ;;
esac
case "$probe_renderer" in
    ''|*[!A-Za-z0-9_-]*)
        echo "SDL_PS5AGC_PROBE_RENDERER must be a renderer driver name" >&2
        exit 2
        ;;
esac
if [ ! -f "$elf" ] || [ ! -f "$bmp" ]; then
    echo "missing testyuv ELF or BMP under $build_dir/test" >&2
    exit 2
fi
if [ ! -d "$pyps4debug_dir" ] || [ ! -f "$killer" ] ||
   ! command -v curl >/dev/null 2>&1 ||
   ! command -v nc >/dev/null 2>&1 ||
   ! command -v uv >/dev/null 2>&1; then
    echo "curl, nc, uv, ps5debug-NG, and the exact-process helper are required" >&2
    exit 2
fi
if ! curl -sS --connect-timeout 3 --max-time 5 \
    "http://${PS5_HOST}:8080/" >/dev/null; then
    echo "PS5 WebSrv is unreachable at ${PS5_HOST}:8080" >&2
    exit 1
fi

kill_eboot() {
    uv run --project "$pyps4debug_dir" python "$killer" "$PS5_HOST" eboot.bin
}

assert_eboot_absent() {
    uv run --project "$pyps4debug_dir" python "$killer" \
        --assert-absent "$PS5_HOST" eboot.bin
}

latest_eboot_pid() {
    sed -n 's/^<\([0-9][0-9]*\)> EXEC \/app0\/eboot\.bin .*category=native_game.*/\1/p' \
        "$1" | tail -n 1
}

sanitize_klog() {
    sanitized=$1.sanitized
    tr -d '\000' <"$1" >"$sanitized"
    mv "$sanitized" "$1"
}

# A stale native-game process makes VideoOut ownership and klog attribution
# ambiguous. Kill it before uploading, then prove it is absent.
kill_eboot || true
assert_eboot_absent

curl -sS --connect-timeout 3 --max-time 10 \
    "ftp://${PS5_HOST}:2121/" --quote "MKD $remote_dir" >/dev/null 2>&1 || true
curl -sS --connect-timeout 3 --max-time 30 -T "$elf" \
    "ftp://${PS5_HOST}:2121${remote_dir}/eboot.elf"
curl -sS --connect-timeout 3 --max-time 30 -T "$bmp" \
    "ftp://${PS5_HOST}:2121${remote_dir}/testyuv.bmp"

mkdir -p "$log_dir"
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
log=$log_dir/${timestamp}-display-probe.log
klog=$log_dir/${timestamp}-display-probe.klog
target_klog=$log_dir/${timestamp}-display-probe-target.klog

launch_status=0
curl -sS --connect-timeout 3 --max-time "$websrv_timeout" --get \
    "http://${PS5_HOST}:8080/hbldr" \
    --data-urlencode pipe=1 \
    --data-urlencode daemon=0 \
    --data-urlencode "path=${remote_dir}/eboot.elf" \
    --data-urlencode "cwd=$remote_dir" \
    --data-urlencode "args=--display-probe --renderer ${probe_renderer} --frames ${probe_frames} ${remote_dir}/testyuv.bmp" \
    >"$log" 2>&1 || launch_status=$?

sleep 2
nc -w 5 "$PS5_HOST" "$klog_port" >"$klog" 2>&1 || true
if [ -s "$klog" ]; then
    sanitize_klog "$klog"
fi

if [ "$launch_status" -ne 0 ]; then
    kill_eboot || true
    sed -n '1,160p' "$log" >&2
    echo "display probe launch failed with curl status $launch_status; log: $log" >&2
    exit 1
fi
sed -n '1,160p' "$log"
if ! grep -F "Renderer selected: ${probe_renderer}" "$log" >/dev/null ||
   ! grep -F 'GPU center pixel: 0xff0000ff' "$log" >/dev/null ||
   grep -E 'VideoOut readback mismatch|GPU center readback failed' "$log" >/dev/null; then
    kill_eboot || true
    echo "display probe did not produce the exact readback oracle; log: $log" >&2
    exit 1
fi
if [ ! -s "$klog" ]; then
    echo "display probe passed but klog capture failed: $klog" >&2
    exit 1
fi

target_pid=$(latest_eboot_pid "$klog")
if [ -z "$target_pid" ]; then
    echo "klog did not identify the display probe PID: $klog" >&2
    exit 1
fi
target_exec_line=$(grep -n "^<${target_pid}> EXEC /app0/eboot\.bin " "$klog" |
    tail -n 1 | cut -d: -f1)
sed -n "${target_exec_line},\$p" "$klog" >"$target_klog"
target_pid_hex=$(printf '%x' "$target_pid")
if grep -Eq \
    "# proc ID: *${target_pid}$|mDBG: Sending signal\(pid: *${target_pid},|App Crash : PID=0x0*${target_pid_hex}([^0-9a-f]|$)|SYSTEM_XO_VIOLATION" \
    "$target_klog" ||
   grep -Eq '=== Reset GFX queue|#### GPU reset sequence starts' \
    "$target_klog"; then
    kill_eboot || true
    echo "display probe hit a fatal or GPU-reset event: $target_klog" >&2
    exit 1
fi
self_kill_line=$(grep -n 'KillApp() appId=' "$target_klog" |
    tail -n 1 | cut -d: -f1 || true)
all_exited_line=$(grep -n '\[AppMgr\] All processes exited' "$target_klog" |
    tail -n 1 | cut -d: -f1 || true)
if [ -z "$self_kill_line" ] || [ -z "$all_exited_line" ] ||
   [ "$all_exited_line" -le "$self_kill_line" ]; then
    kill_eboot || true
    echo "display probe lifecycle evidence is incomplete: $target_klog" >&2
    exit 1
fi
warning='[KERNEL] WARNING: VM resource leak: set:1, res:0, amount:0x4000'
warning_count=$(grep -Fxc "$warning" "$target_klog" || true)
if grep -F '[KERNEL] WARNING:' "$target_klog" | grep -Fvx "$warning" \
    >/dev/null || [ "$warning_count" -gt 1 ]; then
    echo "display probe produced an unexpected kernel warning: $target_klog" >&2
    exit 1
fi
assert_eboot_absent
if ! curl -sS --connect-timeout 3 --max-time 5 \
    "http://${PS5_HOST}:8080/" >/dev/null; then
    echo "display probe exited but WebSrv became unreachable" >&2
    exit 1
fi

echo "ps5agc display probe: PASS renderer=$probe_renderer pixel=0xff0000ff frames=$probe_frames pid=$target_pid"
echo "log: $log"
echo "klog: $target_klog"
