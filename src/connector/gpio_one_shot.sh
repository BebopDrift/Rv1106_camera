#!/bin/sh
# Runs on the board inside this invocation's private directory.
set -eu
level=/sys/bus/platform/drivers/qu-hello-platform/hello-platform/level
shot=/tmp/gpio-shot.nv12

fail() { echo "$*" >&2; exit 1; }
[ -c /dev/hello-gpio-events ] && [ -w "$level" ] || fail "GPIO interface missing: check hello_platform and $level"
[ -c /dev/video11 ] || fail "SC3336 capture device /dev/video11 missing"
# The existing capture program publishes the snapshot at a fixed path by rename().
# Serialize users of this runner and pin the published inode before processing.
exec 9>/tmp/connector-gpio.lock
flock -n 9 || fail "cannot lock /tmp/connector-gpio.lock for capture"
pid=
aiq_pid=
stop_child() {
    [ -n "$1" ] || return 0
    if kill "$1" 2>/dev/null; then
        sleep 1
        kill -KILL "$1" 2>/dev/null || :
    fi
    wait "$1" 2>/dev/null || :
}
cleanup() {
    printf '0\n' > "$level" || :
    stop_child "$pid"
    stop_child "$aiq_pid"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
printf '0\n' > "$level"
[ "$(cat "$level")" = 0 ] || fail "GPIO loopback input did not return low"
if [ -e "$shot" ]; then ln "$shot" before.nv12; fi

# Use the established 3A service if present. Only stop a process started by this run.
if ! pidof rkaiq_3A_server >/dev/null; then
    aiq=$(command -v rkaiq_3A_server || echo /oem/usr/bin/rkaiq_3A_server)
    [ -x "$aiq" ] || fail "rkaiq_3A_server missing; restore the established SC3336/AIQ capture setup"
    if pidof rkipc >/dev/null || pidof luckfox_camera >/dev/null; then
        fail "camera service active; use the established exclusive /dev/video11 capture setup"
    fi
    "$aiq" 9>&- >aiq.log 2>&1 &
    aiq_pid=$!
    sleep 1
    if ! pidof rkaiq_3A_server >/dev/null; then cat aiq.log; fail "AIQ server exited"; fi
fi

# This board's BusyBox may lack the timeout applet. Track and reap our own children.
finish_child() {
    remaining=$1
    while kill -0 "$pid" 2>/dev/null && [ "$remaining" -gt 0 ]; do
        sleep 1
        remaining=$((remaining-1))
    done
    if kill -0 "$pid" 2>/dev/null; then cat "$2"; fail "$3 timed out"; fi
    rc=0
    wait "$pid" || rc=$?
    pid=
    cat "$2"
    [ "$rc" -eq 0 ] || fail "$3 failed (exit $rc)"
}

./gpio_v4l2_capture >capture.log 2>&1 &
pid=$!
for attempt in 1 2 3 4 5 6 7 8; do
    grep -qx CAPTURE_READY capture.log && break
    kill -0 "$pid" 2>/dev/null || break
    sleep 1
done
if ! grep -qx CAPTURE_READY capture.log; then cat capture.log; fail "CAPTURE_READY missing"; fi
sleep 3
printf '1\n' > "$level"
finish_child 15 capture.log capture
printf '0\n' > "$level"
grep -qx 'format=NV12 2304x1296 stride=2304 sizeimage=4478976' capture.log || fail "unexpected capture format"
grep -qx 'SAVED path=/tmp/gpio-shot.nv12 bytes=4478976' capture.log || fail "capture did not save a frame"
grep -qx GPIO_V4L2_TEST_OK capture.log || fail "capture incomplete"
# Extract the event and frame numbers from this run; verify the trigger matched.
ids=$(awk '
    /^TRIGGER / {for(i=2;i<=NF;i++) if($i~/^gpio_seq=[0-9]+$/) t=substr($i,10); nt++}
    /^MATCH / {for(i=2;i<=NF;i++) {
        if($i~/^gpio_seq=[0-9]+$/) m=substr($i,10)
        if($i~/^frame_seq=[0-9]+$/) f=substr($i,11)
    } nm++}
    END {if(nt!=1 || nm!=1 || t=="" || m=="" || f=="" || t!=m) exit 1
         print "gpio_seq=" t " frame_seq=" f}
' capture.log) || fail "missing or inconsistent TRIGGER/MATCH"
ln "$shot" current.nv12
if [ -e before.nv12 ] && [ before.nv12 -ef current.nv12 ]; then fail "stale snapshot"; fi
rm -f before.nv12
./nv12_to_rgb current.nv12 frame.rgb
export LD_LIBRARY_PATH="/oem/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
./connector_one_frame connector_rv1106.rknn frame.rgb RK_anchors.txt >infer.log 2>&1 &
pid=$!
finish_child 15 infer.log inference
awk -v ids="$ids" -f inspection_result.awk infer.log
