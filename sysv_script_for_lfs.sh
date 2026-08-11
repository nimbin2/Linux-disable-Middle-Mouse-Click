#!/bin/sh
### BEGIN INIT INFO
# Provides:          middle_button_disable
# Required-Start:    $local_fs $remote_fs udev
# Required-Stop:     $local_fs $remote_fs
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Disable TrackPoint middle click (keep scrolling)
### END INIT INFO

DAEMON=/usr/local/sbin/middle_button_disable
NAME=middle_button_disable
PIDFILE=/run/$NAME.pid
LOGFILE=/var/log/$NAME.log

# Choose ONE:
ARGS="--auto"
# ARGS="-d /dev/input/event13 -d /dev/input/event15"

is_running()
{
    [ -f "$PIDFILE" ] || return 1
    PID="$(cat "$PIDFILE" 2>/dev/null)" || return 1
    [ -n "$PID" ] || return 1
    kill -0 "$PID" 2>/dev/null
}

do_start()
{
    echo "Starting $NAME..."

    if [ ! -x "$DAEMON" ]; then
        echo "Error: $DAEMON not found or not executable"
        exit 1
    fi

    if is_running; then
        echo "$NAME already running (pid $(cat "$PIDFILE"))."
        exit 0
    fi

    # Start in background and write pidfile.
    # nohup ensures it survives boot script environment.
    umask 022
    nohup "$DAEMON" $ARGS >>"$LOGFILE" 2>&1 &
    PID=$!
    echo "$PID" > "$PIDFILE"

    # quick sanity check
    sleep 0.1
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "Failed to start $NAME (see $LOGFILE)"
        rm -f "$PIDFILE"
        exit 1
    fi
}

do_stop()
{
    echo "Stopping $NAME..."
    if ! is_running; then
        echo "$NAME is not running."
        rm -f "$PIDFILE"
        exit 0
    fi

    PID="$(cat "$PIDFILE")"
    kill "$PID" 2>/dev/null || true

    # Wait up to ~5 seconds
    i=0
    while kill -0 "$PID" 2>/dev/null; do
        i=$((i+1))
        [ "$i" -ge 50 ] && break
        sleep 0.1
    done

    if kill -0 "$PID" 2>/dev/null; then
        echo "Still running, sending SIGKILL..."
        kill -9 "$PID" 2>/dev/null || true
    fi

    rm -f "$PIDFILE"
}

case "$1" in
  start)   do_start ;;
  stop)    do_stop ;;
  restart) do_stop; sleep 1; do_start ;;
  status)
    if is_running; then
      echo "$NAME is running (pid $(cat "$PIDFILE"))."
      exit 0
    else
      echo "$NAME is not running."
      exit 3
    fi
    ;;
  *)
    echo "Usage: $0 {start|stop|restart|status}"
    exit 2
    ;;
esac

exit 0
