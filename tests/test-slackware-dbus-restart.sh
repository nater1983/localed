#!/bin/sh
# Exercise locale1 recovery on an isolated system bus.
# Usage: tests/test-slackware-dbus-restart.sh /path/to/localed
set -eu

LOCALed=${1:-}
if [ -z "$LOCALed" ] || [ ! -x "$LOCALed" ]; then
  echo "usage: $0 /path/to/localed" >&2
  exit 2
fi

for tool in dbus-daemon gdbus grep script pgrep; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "missing required tool: $tool" >&2
    exit 2
  }
done

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/localed-dbus-test.XXXXXX")
socket="$tmpdir/system-bus"
log="$tmpdir/localed.log"
bus_log="$tmpdir/dbus.log"
BUS_ADDRESS="unix:path=$socket"
bus_pid=
localed_pid=
script_pid=
trace_pid=
trace_log="$tmpdir/localed-reconnect.strace"

cleanup()
{
  if [ -n "$localed_pid" ]; then
    kill "$localed_pid" 2>/dev/null || :
  fi
  if [ -n "$script_pid" ]; then
    kill "$script_pid" 2>/dev/null || :
    wait "$script_pid" 2>/dev/null || :
  fi
  if [ -n "$trace_pid" ]; then
    kill "$trace_pid" 2>/dev/null || :
    wait "$trace_pid" 2>/dev/null || :
  fi
  if [ -n "$bus_pid" ]; then
    kill "$bus_pid" 2>/dev/null || :
    wait "$bus_pid" 2>/dev/null || :
  fi
  rm -rf "$tmpdir"
}
trap cleanup EXIT HUP INT TERM

start_bus()
{
  rm -f "$socket"
  dbus-daemon --session --nofork --nopidfile \
    --address="$BUS_ADDRESS" >"$bus_log" 2>&1 &
  bus_pid=$!
  i=0
  while [ ! -S "$socket" ]; do
    i=$((i + 1))
    if [ "$i" -ge 100 ] || ! kill -0 "$bus_pid" 2>/dev/null; then
      echo "private D-Bus daemon failed to start" >&2
      cat "$bus_log" >&2
      exit 1
    fi
    sleep 0.05
  done
}

wait_for_locale1()
{
  i=0
  while ! DBUS_SYSTEM_BUS_ADDRESS="$BUS_ADDRESS" \
      gdbus introspect --system \
        --dest org.freedesktop.locale1 \
        --object-path /org/freedesktop/locale1 >/dev/null 2>&1; do
    i=$((i + 1))
    if [ "$i" -ge 120 ]; then
      echo "localed did not reacquire org.freedesktop.locale1" >&2
      cat "$log" >&2
      exit 1
    fi
    sleep 0.1
  done
}

wait_for_log()
{
  pattern=$1
  i=0
  while ! grep -F "$pattern" "$log" >/dev/null 2>&1; do
    i=$((i + 1))
    if [ "$i" -ge 100 ]; then
      echo "timed out waiting for log: $pattern" >&2
      cat "$log" >&2
      exit 1
    fi
    sleep 0.1
  done
}

export DBUS_SYSTEM_BUS_ADDRESS="$BUS_ADDRESS"
start_bus

# localed routes logs to syslog when stderr is not a terminal. Run it under
# a PTY so --debug messages are captured in the typescript for assertions.
# Exercise --replace as a one-time startup instruction. Once locale1 is
# acquired, the daemon must reconnect normally after the bus restarts.
script -q -f -e -c "exec \"$LOCALed\" --debug --replace" "$log" &
script_pid=$!
i=0
while [ -z "$localed_pid" ]; do
  localed_pid=$(pgrep -P "$script_pid" -x localed 2>/dev/null || :)
  i=$((i + 1))
  if [ "$i" -ge 100 ] || ! kill -0 "$script_pid" 2>/dev/null; then
    echo "localed failed to start under script(1)" >&2
    cat "$log" >&2
    exit 1
  fi
  sleep 0.05
done

echo "1/3 waiting for initial locale1 name"
wait_for_locale1

echo "2/3 attaching process diagnostics before bus restart"
ps -o pid,ppid,stat,wchan:32,cmd -p "$localed_pid" || :
if command -v strace >/dev/null 2>&1; then
  strace -f -tt -k -e signal=all -p "$localed_pid" \
    -e trace=poll,ppoll,epoll_wait,epoll_pwait,futex,read,write,kill,tkill,tgkill,rt_sigqueueinfo,rt_tgsigqueueinfo \
    -o "$trace_log" &
  trace_pid=$!
  sleep 0.2
else
  echo "strace not installed; skipping process syscall capture"
fi

echo "3/3 restarting the isolated system bus"
kill "$bus_pid"
wait "$bus_pid" 2>/dev/null || :
bus_pid=
start_bus

echo "verifying locale1 reconnection"
sleep 5
if [ -n "$trace_pid" ]; then
  kill "$trace_pid" 2>/dev/null || :
  wait "$trace_pid" 2>/dev/null || :
  trace_pid=
  echo "Signal delivery and sender syscalls:"
  grep -n -B 8 -A 12 -E 'kill|tkill|tgkill|rt_sigqueueinfo|rt_tgsigqueueinfo|SIGTERM|SIGINT' "$trace_log" || :
  echo "Recent localed system-call activity:"
  tail -100 "$trace_log" || :
fi
if ! kill -0 "$localed_pid" 2>/dev/null; then
  echo "localed exited during the system-bus restart" >&2
  cat "$log" >&2
  exit 1
fi
if [ -s "$trace_log" ] && grep -E -- '--- SIGTERM|tgkill.*SIGTERM|raise.*SIGTERM' "$trace_log" >/dev/null 2>&1; then
  echo "localed received or generated SIGTERM during the bus restart" >&2
  cat "$trace_log" >&2
  exit 1
fi
ps -o pid,ppid,stat,wchan:32,cmd -p "$localed_pid" || :
wait_for_log "D-Bus reconnect timer fired"
wait_for_locale1
echo "PASS: locale1 name reacquired after system-bus restart"
