#!/bin/sh
set -eu

: "${PS5_HOST:?set PS5_HOST to the FW 5.50 console address}"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(dirname -- "$script_dir")
build_dir=${SDL_PS5AGC_BUILD_DIR:-$repo_dir/build-ps5agc-native2}
probe_kind=${SDL_PS5AGC_PROBE_KIND:-display}
automation_filter=${SDL_PS5AGC_AUTOMATION_FILTER:-Render}
if [ "$probe_kind" = automation ]; then
    default_elf=$build_dir/test/testautomation
else
    default_elf=$build_dir/test/testyuv
fi
elf=${SDL_PS5AGC_PROBE_ELF:-$default_elf}
bmp=${SDL_PS5AGC_PROBE_BMP:-$build_dir/test/testyuv.bmp}
websrv_timeout=${SDL_PS5AGC_WEBSRV_TIMEOUT:-30}
probe_frames=${SDL_PS5AGC_PROBE_FRAMES:-1}
probe_renderer=${SDL_PS5AGC_PROBE_RENDERER:-ps5agc}
expected_renderer=${SDL_PS5AGC_EXPECT_RENDERER:-}
yuv_format=${SDL_PS5AGC_YUV_FORMAT:-yv12}
yuv_mode=${SDL_PS5AGC_YUV_MODE:-jpeg}
probe_accelerated=${SDL_PS5AGC_PROBE_ACCELERATED:-0}
expect_failure=${SDL_PS5AGC_EXPECT_FAILURE:-0}
expected_error=${SDL_PS5AGC_EXPECT_ERROR:-}
klog_port=${SDL_PS5AGC_KLOG_PORT:-3232}
pyps4debug_dir=${PYPS4DEBUG_DIR:-/Users/bizkut/Downloads/PS5/homebrew/PyPS4debug}
killer=${PS5DEBUG_KILLER:-$repo_dir/../Vulkan-PS5/examples/ps5debug_kill_process.py}
log_dir=${SDL_PS5AGC_LOG_DIR:-${TMPDIR:-/tmp}/sdl-ps5agc-qualification}
remote_dir=/data/homebrew/sdl_ps5agc_display_probe

if [ -z "$expected_renderer" ]; then
    if [ "$probe_renderer" = auto ]; then
        expected_renderer=ps5agc
    else
        expected_renderer=$probe_renderer
    fi
fi

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
    auto) ;;
    ''|*[!A-Za-z0-9_-]*)
        echo "SDL_PS5AGC_PROBE_RENDERER must be 'auto' or a renderer driver name" >&2
        exit 2
        ;;
esac
case "$expected_renderer" in
    ''|*[!A-Za-z0-9_-]*)
        echo "SDL_PS5AGC_EXPECT_RENDERER must be a renderer driver name" >&2
        exit 2
        ;;
esac
case "$probe_kind" in
    automation|display|yuv) ;;
    *)
        echo "SDL_PS5AGC_PROBE_KIND must be automation, display, or yuv" >&2
        exit 2
        ;;
esac
case "$automation_filter" in
    ''|*[!A-Za-z0-9_]* )
        echo "SDL_PS5AGC_AUTOMATION_FILTER must be a suite or test name" >&2
        exit 2
        ;;
esac
case "$yuv_format" in
    iyuv|yv12|nv12|nv21) ;;
    *)
        echo "SDL_PS5AGC_YUV_FORMAT must be iyuv, yv12, nv12, or nv21" >&2
        exit 2
        ;;
esac
case "$yuv_mode" in
    jpeg|bt601|bt709) ;;
    *)
        echo "SDL_PS5AGC_YUV_MODE must be jpeg, bt601, or bt709" >&2
        exit 2
        ;;
esac
case "$probe_accelerated" in
    0|1) ;;
    *)
        echo "SDL_PS5AGC_PROBE_ACCELERATED must be 0 or 1" >&2
        exit 2
        ;;
esac
case "$expect_failure" in
    0|1) ;;
    *)
        echo "SDL_PS5AGC_EXPECT_FAILURE must be 0 or 1" >&2
        exit 2
        ;;
esac
if [ "$expect_failure" -eq 1 ] && [ -z "$expected_error" ]; then
    echo "SDL_PS5AGC_EXPECT_ERROR is required for an expected failure" >&2
    exit 2
fi
if [ ! -f "$elf" ]; then
    echo "missing probe ELF: $elf" >&2
    exit 2
fi
if [ "$probe_kind" != automation ] && [ ! -f "$bmp" ]; then
    echo "missing testyuv BMP: $bmp" >&2
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
    uv run --project "$pyps4debug_dir" python "$killer" "$PS5_HOST" eboot.elf
    uv run --project "$pyps4debug_dir" python "$killer" "$PS5_HOST" eboot.bin
}

assert_eboot_absent() {
    uv run --project "$pyps4debug_dir" python "$killer" \
        --assert-absent "$PS5_HOST" eboot.elf
    uv run --project "$pyps4debug_dir" python "$killer" \
        --assert-absent "$PS5_HOST" eboot.bin
}

remove_eboot() {
    kill_eboot || true
    assert_eboot_absent
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
remove_eboot

curl -sS --connect-timeout 3 --max-time 10 \
    "ftp://${PS5_HOST}:2121/" --quote "MKD $remote_dir" >/dev/null 2>&1 || true
curl -sS --connect-timeout 3 --max-time 30 -T "$elf" \
    "ftp://${PS5_HOST}:2121${remote_dir}/eboot.elf"
if [ "$probe_kind" != automation ]; then
    curl -sS --connect-timeout 3 --max-time 30 -T "$bmp" \
        "ftp://${PS5_HOST}:2121${remote_dir}/testyuv.bmp"
fi

mkdir -p "$log_dir"
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
log=$log_dir/${timestamp}-display-probe.log
klog=$log_dir/${timestamp}-display-probe.klog
target_klog=$log_dir/${timestamp}-display-probe-target.klog

launch_status=0
if [ "$probe_kind" = automation ]; then
    probe_args="--filter ${automation_filter}"
elif [ "$probe_kind" = yuv ]; then
    probe_args="--yuv-update-probe --${yuv_format} --${yuv_mode}"
else
    probe_args=--display-probe
fi
if [ "$probe_kind" = automation ]; then
    launch_args=$probe_args
    if [ "$probe_renderer" != auto ]; then
        launch_args="${probe_args} --renderer ${probe_renderer}"
    fi
else
    launch_args="${probe_args} --frames ${probe_frames} ${remote_dir}/testyuv.bmp"
    if [ "$probe_renderer" != auto ]; then
        launch_args="${probe_args} --renderer ${probe_renderer} --frames ${probe_frames} ${remote_dir}/testyuv.bmp"
    fi
fi
if [ "$probe_accelerated" -eq 1 ]; then
    launch_args="--accelerated ${launch_args}"
fi
curl -sS --connect-timeout 3 --max-time "$websrv_timeout" --get \
    "http://${PS5_HOST}:8080/hbldr" \
    --data-urlencode pipe=1 \
    --data-urlencode daemon=0 \
    --data-urlencode "path=${remote_dir}/eboot.elf" \
    --data-urlencode "cwd=$remote_dir" \
    --data-urlencode "args=${launch_args}" \
    >"$log" 2>&1 || launch_status=$?

sleep 2
nc -w 5 "$PS5_HOST" "$klog_port" >"$klog" 2>&1 || true
if [ -s "$klog" ]; then
    sanitize_klog "$klog"
fi

if [ "$launch_status" -ne 0 ]; then
    remove_eboot
    sed -n '1,160p' "$log" >&2
    echo "display probe launch failed with curl status $launch_status; log: $log" >&2
    exit 1
fi
if [ ! -s "$klog" ]; then
    remove_eboot
    echo "display probe completed but klog capture failed: $klog" >&2
    exit 1
fi

target_pid=$(latest_eboot_pid "$klog")
if [ -z "$target_pid" ]; then
    remove_eboot
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
    "$target_klog" ||
   grep -Eq 'PowerManager\.RequestStateChange state:(Reboot|Shutdown)|Start SystemReboot|Start SystemShutdown' \
    "$target_klog"; then
    remove_eboot
    echo "display probe hit a fatal, GPU-reset, or system power event: $target_klog" >&2
    exit 1
fi

sed -n '1,160p' "$log"
if [ "$expect_failure" -eq 1 ]; then
    if ! grep -F -- "$expected_error" "$log" >/dev/null ||
       grep -F 'GPU center pixel:' "$log" >/dev/null; then
        remove_eboot
        echo "display probe did not produce the expected failure; log: $log" >&2
        exit 1
    fi
else
    oracle_failed=0
    if [ "$probe_kind" = automation ]; then
        if ! grep -F "Render suite renderer: ${expected_renderer}" "$log" >/dev/null ||
           ! grep -F 'Exit code: 0' "$log" >/dev/null ||
           grep -E ">>> (Test|Suite).*: Failed|Failed=[1-9]" "$log" >/dev/null; then
            oracle_failed=1
        fi
        if [ "$automation_filter" = Render ]; then
            if ! grep -F 'Suite Summary: Total=7 Passed=4 Failed=0 Skipped=3' "$log" >/dev/null ||
               ! grep -F ">>> Suite 'Render': Passed" "$log" >/dev/null; then
                oracle_failed=1
            fi
        elif ! grep -F ">>> Test '${automation_filter}': Passed" "$log" >/dev/null ||
             ! grep -F 'Run Summary: Total=1 Passed=1 Failed=0 Skipped=0' "$log" >/dev/null; then
            oracle_failed=1
        fi
    elif [ "$probe_kind" = yuv ]; then
        if ! grep -F 'YUV update probe: PASS' "$log" >/dev/null ||
           ! grep -F 'YUV odd update rect=1,1 553x331 pitches=' "$log" >/dev/null ||
           grep -E 'YUV update probe mismatch|GPU center readback failed' "$log" >/dev/null; then
            oracle_failed=1
        fi
    elif ! grep -F 'GPU center pixel: 0xff0000ff' "$log" >/dev/null ||
         grep -E 'VideoOut readback mismatch|GPU center readback failed' "$log" >/dev/null; then
        oracle_failed=1
    fi
    if [ "$probe_kind" != automation ] &&
       ! grep -F "Renderer selected: ${expected_renderer}" "$log" >/dev/null; then
        oracle_failed=1
    fi
    if [ "$oracle_failed" -ne 0 ]; then
        remove_eboot
        echo "display probe did not produce the expected renderer and readback oracle; log: $log" >&2
        exit 1
    fi
fi
self_kill_line=$(grep -n 'KillApp() appId=' "$target_klog" |
    tail -n 1 | cut -d: -f1 || true)
all_exited_line=$(grep -n '\[AppMgr\] All processes exited' "$target_klog" |
    tail -n 1 | cut -d: -f1 || true)
if [ -z "$self_kill_line" ] || [ -z "$all_exited_line" ] ||
   [ "$all_exited_line" -le "$self_kill_line" ]; then
    remove_eboot
    echo "display probe lifecycle evidence is incomplete: $target_klog" >&2
    exit 1
fi
warning='[KERNEL] WARNING: VM resource leak: set:1, res:0, amount:0x4000'
warning_count=$(grep -Fxc "$warning" "$target_klog" || true)
if grep -F '[KERNEL] WARNING:' "$target_klog" | grep -Fvx "$warning" \
    >/dev/null || [ "$warning_count" -gt 1 ]; then
    remove_eboot
    echo "display probe produced an unexpected kernel warning: $target_klog" >&2
    exit 1
fi
assert_eboot_absent
if ! curl -sS --connect-timeout 3 --max-time 5 \
    "http://${PS5_HOST}:8080/" >/dev/null; then
    echo "display probe exited but WebSrv became unreachable" >&2
    exit 1
fi

if [ "$expect_failure" -eq 1 ]; then
    echo "ps5agc display probe: PASS expected-failure renderer=$probe_renderer accelerated=$probe_accelerated frames=$probe_frames pid=$target_pid"
else
    if [ "$probe_kind" = automation ]; then
        echo "ps5agc display probe: PASS kind=automation filter=$automation_filter requested=$probe_renderer selected=$expected_renderer pid=$target_pid"
    elif [ "$probe_kind" = yuv ]; then
        echo "ps5agc display probe: PASS kind=yuv format=$yuv_format mode=$yuv_mode requested=$probe_renderer selected=$expected_renderer accelerated=$probe_accelerated frames=$probe_frames pid=$target_pid"
    else
        echo "ps5agc display probe: PASS kind=display requested=$probe_renderer selected=$expected_renderer accelerated=$probe_accelerated pixel=0xff0000ff frames=$probe_frames pid=$target_pid"
    fi
fi
echo "log: $log"
echo "klog: $target_klog"
