#!/bin/sh
set -eu

: "${PS5_HOST:?set PS5_HOST to the FW 5.50 console address}"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(dirname -- "$script_dir")
build_dir=${SDL_PS5AGC_BUILD_DIR:-$repo_dir/build-ps5agc-native2}
probe_kind=${SDL_PS5AGC_PROBE_KIND:-display}
automation_filter=${SDL_PS5AGC_AUTOMATION_FILTER:-Render}
standalone_target=${SDL_PS5AGC_STANDALONE_TARGET:-}
failure_point=${SDL_PS5AGC_FAILURE_POINT:-}
if [ "$probe_kind" = automation ]; then
    probe_target=testautomation
    default_elf=$build_dir/test/testautomation
elif [ "$probe_kind" = standalone ]; then
    probe_target=$standalone_target
    default_elf=$build_dir/test/$standalone_target
elif [ "$probe_kind" = failure ]; then
    probe_target=testps5agcfailure
    default_elf=$build_dir/test/testps5agcfailure
else
    probe_target=testyuv
    default_elf=$build_dir/test/testyuv
fi
elf=${SDL_PS5AGC_PROBE_ELF:-$default_elf}
bmp=${SDL_PS5AGC_PROBE_BMP:-$build_dir/test/testyuv.bmp}
websrv_timeout=${SDL_PS5AGC_WEBSRV_TIMEOUT:-30}
probe_frames=${SDL_PS5AGC_PROBE_FRAMES:-1}
recreate_count=${SDL_PS5AGC_RECREATE_COUNT:-8}
texture_churn_count=${SDL_PS5AGC_TEXTURE_CHURN_COUNT:-32}
probe_renderer=${SDL_PS5AGC_PROBE_RENDERER:-ps5agc}
expected_renderer=${SDL_PS5AGC_EXPECT_RENDERER:-}
yuv_format=${SDL_PS5AGC_YUV_FORMAT:-yv12}
yuv_mode=${SDL_PS5AGC_YUV_MODE:-jpeg}
probe_accelerated=${SDL_PS5AGC_PROBE_ACCELERATED:-0}
expect_failure=${SDL_PS5AGC_EXPECT_FAILURE:-0}
expected_error=${SDL_PS5AGC_EXPECT_ERROR:-}
skip_build=${SDL_PS5AGC_SKIP_BUILD:-0}
build_jobs=${SDL_PS5AGC_BUILD_JOBS:-4}
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
    ''|*[!0-9]*|0*)
        echo "SDL_PS5AGC_WEBSRV_TIMEOUT must be a positive integer" >&2
        exit 2
        ;;
esac
case "$probe_frames" in
    ''|*[!0-9]*|0*)
        echo "SDL_PS5AGC_PROBE_FRAMES must be a positive integer" >&2
        exit 2
        ;;
esac
case "$recreate_count" in
    ''|*[!0-9]*|0*)
        echo "SDL_PS5AGC_RECREATE_COUNT must be a positive integer" >&2
        exit 2
        ;;
esac
case "$texture_churn_count" in
    ''|*[!0-9]*|0*)
        echo "SDL_PS5AGC_TEXTURE_CHURN_COUNT must be a positive integer" >&2
        exit 2
        ;;
    *)
        if [ "$texture_churn_count" -gt 1000 ]; then
            echo "SDL_PS5AGC_TEXTURE_CHURN_COUNT must not exceed 1000" >&2
            exit 2
        fi
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
    automation|blend|churn|display|failure|packed|recreate|standalone|target|yuv) ;;
    *)
        echo "SDL_PS5AGC_PROBE_KIND must be automation, blend, churn, display, failure, packed, recreate, standalone, target, or yuv" >&2
        exit 2
        ;;
esac
if [ "$probe_kind" = standalone ]; then
    case "$standalone_target" in
        testgeometry|testrendercopyex|testrendertarget|testscale|testsprite2) ;;
        *)
            echo "SDL_PS5AGC_STANDALONE_TARGET must name one supported renderer test" >&2
            exit 2
            ;;
    esac
fi
if [ "$probe_kind" = failure ]; then
    case "$failure_point" in
        mode-query|initialization|allocation|submission|presentation) ;;
        *)
            echo "SDL_PS5AGC_FAILURE_POINT must name one supported failpoint" >&2
            exit 2
            ;;
    esac
fi
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
if { [ "$probe_kind" = standalone ] || [ "$probe_kind" = failure ]; } &&
   [ "$probe_accelerated" -ne 0 ]; then
    echo "SDL_PS5AGC_PROBE_ACCELERATED is not supported by this probe kind" >&2
    exit 2
fi
case "$expect_failure" in
    0|1) ;;
    *)
        echo "SDL_PS5AGC_EXPECT_FAILURE must be 0 or 1" >&2
        exit 2
        ;;
esac
case "$skip_build" in
    0|1) ;;
    *)
        echo "SDL_PS5AGC_SKIP_BUILD must be 0 or 1" >&2
        exit 2
        ;;
esac
case "$build_jobs" in
    ''|*[!0-9]*|0*)
        echo "SDL_PS5AGC_BUILD_JOBS must be a positive integer" >&2
        exit 2
        ;;
esac
if [ "$expect_failure" -eq 1 ] && [ -z "$expected_error" ]; then
    echo "SDL_PS5AGC_EXPECT_ERROR is required for an expected failure" >&2
    exit 2
fi
if [ "$probe_kind" = failure ] &&
   ! grep -F 'SDL_PS5_OPENAGC_TEST_HOOKS:BOOL=ON' "$build_dir/CMakeCache.txt" >/dev/null 2>&1; then
    echo "failure probes require SDL_PS5_OPENAGC_TEST_HOOKS=ON in $build_dir" >&2
    exit 2
fi
if [ "$skip_build" -eq 0 ] && [ "$elf" = "$default_elf" ]; then
    if ! command -v cmake >/dev/null 2>&1; then
        echo "cmake is required to rebuild the default probe ELF" >&2
        exit 2
    fi
    if [ "$probe_kind" = standalone ]; then
        cmake --build "$build_dir" --target "$probe_target" copy-sdl-test-resources -j "$build_jobs"
    else
        cmake --build "$build_dir" --target "$probe_target" -j "$build_jobs"
    fi
fi
if [ ! -f "$elf" ]; then
    echo "missing probe ELF: $elf" >&2
    exit 2
fi
if [ "$probe_kind" != automation ] && [ "$probe_kind" != standalone ] &&
   [ "$probe_kind" != failure ] && [ ! -f "$bmp" ]; then
    echo "missing testyuv BMP: $bmp" >&2
    exit 2
fi
if [ "$probe_kind" = standalone ]; then
    for resource in icon.bmp sample.bmp; do
        if [ ! -f "$build_dir/test/$resource" ]; then
            echo "missing standalone test resource: $build_dir/test/$resource" >&2
            exit 2
        fi
    done
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

assert_eboot_absent() {
    uv run --project "$pyps4debug_dir" python "$killer" \
        --assert-absent "$PS5_HOST" eboot.elf
    uv run --project "$pyps4debug_dir" python "$killer" \
        --assert-absent "$PS5_HOST" eboot.bin
}

wait_eboot_absent() {
    attempt=0
    while [ "$attempt" -lt 40 ]; do
        if assert_eboot_absent >/dev/null 2>&1; then
            return 0
        fi
        attempt=$((attempt + 1))
        sleep 0.25
    done
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
# ambiguous. Refuse to launch rather than debugger-attaching to an app that
# may be in SystemService teardown.
assert_eboot_absent

curl -sS --connect-timeout 3 --max-time 10 \
    "ftp://${PS5_HOST}:2121/" --quote "MKD $remote_dir" >/dev/null 2>&1 || true
curl -sS --connect-timeout 3 --max-time 30 -T "$elf" \
    "ftp://${PS5_HOST}:2121${remote_dir}/eboot.elf"
if [ "$probe_kind" = standalone ]; then
    for resource in icon.bmp sample.bmp; do
        curl -sS --connect-timeout 3 --max-time 30 -T "$build_dir/test/$resource" \
            "ftp://${PS5_HOST}:2121${remote_dir}/$resource"
    done
elif [ "$probe_kind" != automation ] && [ "$probe_kind" != failure ]; then
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
elif [ "$probe_kind" = failure ]; then
    probe_args="--failure ${failure_point}"
elif [ "$probe_kind" = standalone ]; then
    case "$standalone_target" in
        testgeometry)
            probe_args="--frames ${probe_frames} --info render --use-texture"
            ;;
        testsprite2)
            probe_args="--frames ${probe_frames} --info render --iterations ${probe_frames} --use-rendergeometry mode2"
            ;;
        *)
            probe_args="--frames ${probe_frames} --info render"
            ;;
    esac
elif [ "$probe_kind" = yuv ]; then
    probe_args="--yuv-update-probe --${yuv_format} --${yuv_mode}"
elif [ "$probe_kind" = packed ]; then
    probe_args=--packed-texture-probe
elif [ "$probe_kind" = target ]; then
    probe_args=--target-texture-probe
elif [ "$probe_kind" = blend ]; then
    probe_args=--blend-probe
elif [ "$probe_kind" = churn ]; then
    probe_args="--display-probe --texture-churn ${texture_churn_count}"
elif [ "$probe_kind" = recreate ]; then
    probe_args="--display-probe --recreate ${recreate_count}"
else
    probe_args=--display-probe
fi
if [ "$probe_kind" = failure ]; then
    launch_args=$probe_args
elif [ "$probe_kind" = automation ] || [ "$probe_kind" = standalone ]; then
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
    wait_eboot_absent || true
    sed -n '1,160p' "$log" >&2
    echo "display probe launch failed with curl status $launch_status; log: $log" >&2
    exit 1
fi
if [ ! -s "$klog" ]; then
    wait_eboot_absent || true
    echo "display probe completed but klog capture failed: $klog" >&2
    exit 1
fi

target_pid=$(latest_eboot_pid "$klog")
if [ -z "$target_pid" ]; then
    wait_eboot_absent || true
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
    wait_eboot_absent || true
    echo "display probe hit a fatal, GPU-reset, or system power event: $target_klog" >&2
    exit 1
fi

sed -n '1,160p' "$log"
if [ "$expect_failure" -eq 1 ]; then
    if ! grep -F -- "$expected_error" "$log" >/dev/null ||
        grep -F 'GPU center pixel:' "$log" >/dev/null; then
        wait_eboot_absent || true
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
    elif [ "$probe_kind" = failure ]; then
        if ! grep -F "ps5agc failure injection: PASS point=${failure_point}" "$log" >/dev/null; then
            oracle_failed=1
        fi
    elif [ "$probe_kind" = standalone ]; then
        if ! grep -F "Frame limit reached: ${probe_frames}" "$log" >/dev/null ||
           ! awk -v expected="$expected_renderer" '
               /Current renderer:/ { current = 1; next }
               current && index($0, "Renderer " expected ":") { found = 1; exit }
               current && /Renderer [^:]*:/ { exit }
               END { exit(found ? 0 : 1) }
           ' "$log"; then
            oracle_failed=1
        fi
    elif [ "$probe_kind" = yuv ]; then
        if ! grep -F 'YUV update probe: PASS' "$log" >/dev/null ||
           ! grep -F 'YUV odd update rect=1,1 553x331 pitches=' "$log" >/dev/null ||
           grep -E 'YUV update probe mismatch|GPU center readback failed' "$log" >/dev/null; then
            oracle_failed=1
        fi
    elif [ "$probe_kind" = packed ]; then
        if ! grep -F 'Packed texture probe: PASS' "$log" >/dev/null ||
           grep -E 'Packed texture probe mismatch|GPU center readback failed' "$log" >/dev/null; then
            oracle_failed=1
        fi
    elif [ "$probe_kind" = target ]; then
        if ! grep -F 'Target texture probe: PASS' "$log" >/dev/null ||
           grep -E 'Target texture probe mismatch|GPU center readback failed' "$log" >/dev/null; then
            oracle_failed=1
        fi
    elif [ "$probe_kind" = blend ]; then
        if ! grep -F 'Zero-alpha blend probe: PASS' "$log" >/dev/null ||
           grep -E 'Zero-alpha blend probe mismatch|GPU center readback failed' "$log" >/dev/null; then
            oracle_failed=1
        fi
    elif [ "$probe_kind" = churn ]; then
        if ! grep -F "Texture churn: PASS count=${texture_churn_count}" "$log" >/dev/null ||
           ! grep -F 'GPU center pixel: 0xff0000ff' "$log" >/dev/null ||
           grep -E 'Texture churn probe failed|texture churn iteration|VideoOut readback mismatch|GPU center readback failed' "$log" >/dev/null; then
            oracle_failed=1
        fi
    elif [ "$probe_kind" = recreate ]; then
        if ! grep -F "Renderer recreation: PASS count=${recreate_count}" "$log" >/dev/null ||
           ! grep -F 'GPU center pixel: 0xff0000ff' "$log" >/dev/null ||
           grep -E 'Renderer recreation [0-9]+/[0-9]+ (failed|validation failed)|VideoOut readback mismatch|GPU center readback failed' "$log" >/dev/null; then
            oracle_failed=1
        fi
    elif ! grep -F 'GPU center pixel: 0xff0000ff' "$log" >/dev/null ||
         grep -E 'VideoOut readback mismatch|GPU center readback failed' "$log" >/dev/null; then
        oracle_failed=1
    fi
    if [ "$probe_kind" != automation ] && [ "$probe_kind" != standalone ] &&
       [ "$probe_kind" != failure ] &&
       ! grep -F "Renderer selected: ${expected_renderer}" "$log" >/dev/null; then
        oracle_failed=1
    fi
    if [ "$expected_renderer" = ps5agc ] &&
       [ "$probe_kind" != automation ] && [ "$probe_kind" != standalone ] &&
       [ "$probe_kind" != failure ] &&
       ! grep -F 'ps5agc display mode: ' "$log" >/dev/null; then
        oracle_failed=1
    fi
    if [ "$oracle_failed" -ne 0 ]; then
        wait_eboot_absent || true
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
    wait_eboot_absent || true
    echo "display probe lifecycle evidence is incomplete: $target_klog" >&2
    exit 1
fi
warning='[KERNEL] WARNING: VM resource leak: set:1, res:0, amount:0x4000'
warning_count=$(grep -Fxc "$warning" "$target_klog" || true)
if grep -F '[KERNEL] WARNING:' "$target_klog" | grep -Fvx "$warning" \
    >/dev/null || [ "$warning_count" -gt 1 ]; then
    wait_eboot_absent || true
    echo "display probe produced an unexpected kernel warning: $target_klog" >&2
    exit 1
fi
wait_eboot_absent
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
    elif [ "$probe_kind" = failure ]; then
        echo "ps5agc display probe: PASS kind=failure point=$failure_point pid=$target_pid"
    elif [ "$probe_kind" = standalone ]; then
        echo "ps5agc display probe: PASS kind=standalone target=$standalone_target requested=$probe_renderer selected=$expected_renderer frames=$probe_frames pid=$target_pid"
    elif [ "$probe_kind" = yuv ]; then
        echo "ps5agc display probe: PASS kind=yuv format=$yuv_format mode=$yuv_mode requested=$probe_renderer selected=$expected_renderer accelerated=$probe_accelerated frames=$probe_frames pid=$target_pid"
    elif [ "$probe_kind" = packed ]; then
        echo "ps5agc display probe: PASS kind=packed requested=$probe_renderer selected=$expected_renderer accelerated=$probe_accelerated frames=$probe_frames pid=$target_pid"
    elif [ "$probe_kind" = target ]; then
        echo "ps5agc display probe: PASS kind=target requested=$probe_renderer selected=$expected_renderer accelerated=$probe_accelerated frames=$probe_frames pid=$target_pid"
    elif [ "$probe_kind" = blend ]; then
        echo "ps5agc display probe: PASS kind=blend requested=$probe_renderer selected=$expected_renderer accelerated=$probe_accelerated frames=$probe_frames pid=$target_pid"
    elif [ "$probe_kind" = churn ]; then
        echo "ps5agc display probe: PASS kind=churn count=$texture_churn_count requested=$probe_renderer selected=$expected_renderer accelerated=$probe_accelerated frames=$probe_frames pid=$target_pid"
    elif [ "$probe_kind" = recreate ]; then
        echo "ps5agc display probe: PASS kind=recreate count=$recreate_count requested=$probe_renderer selected=$expected_renderer accelerated=$probe_accelerated frames=$probe_frames pid=$target_pid"
    else
        echo "ps5agc display probe: PASS kind=display requested=$probe_renderer selected=$expected_renderer accelerated=$probe_accelerated pixel=0xff0000ff frames=$probe_frames pid=$target_pid"
    fi
fi
echo "log: $log"
echo "klog: $target_klog"
