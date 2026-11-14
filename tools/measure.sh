#!/bin/sh
# Launches the built app and reports resident memory and CPU over a 20 second window (after 5 seconds of start-up).
set -eu
app=build/QuietSnow.app/Contents/MacOS/QuietSnow
"$app" &
pid=$!
sleep 5
cpu() { ps -o cputime= -p "$pid" | awk -F: '{ if (NF == 3) print $1 * 3600 + $2 * 60 + $3; else print $1 * 60 + $2 }'; }
t1=$(cpu)
sleep 20
t2=$(cpu)
[ -n "${SAMPLE_OUT:-}" ] && sample "$pid" 8 -file "$SAMPLE_OUT" >/dev/null 2>&1 || true
rss=$(ps -o rss= -p "$pid")
kill "$pid" 2>/dev/null || true
awk -v a="$t1" -v b="$t2" -v r="$rss" 'BEGIN { printf "rss_mb=%.1f cpu_pct=%.2f\n", r / 1024, (b - a) / 20 * 100 }'
