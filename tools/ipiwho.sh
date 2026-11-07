#!/usr/bin/env bash
# ipiwho.sh - who makes CPUs interrupt each other ("CAL" in /proc/interrupts)?
# Usage: sudo ./ipiwho.sh [seconds]      (default 5; read-only apart from tracing)
#
# Records the kernel tracepoint csd:csd_queue_cpu for a few seconds: every
# time a CPU asks another CPU to run a function (smp_call_function*), it logs
# which function, called from where, by which task. Prints the most frequent
# combinations. flush_tlb_func = TLB shootdowns (munmap/mprotect/migration).

secs="${1:-5}"
(( EUID == 0 )) || { echo "run with sudo" >&2; exit 1; }
T=/sys/kernel/tracing
[[ -d $T/events ]] || mount -t tracefs nodev "$T" 2>/dev/null
[[ -d $T/events ]] || T=/sys/kernel/debug/tracing
ev="$T/events/csd/csd_queue_cpu/enable"
[[ -w "$ev" ]] || { echo "kernel has no csd:csd_queue_cpu tracepoint (needs tracing enabled)" >&2; exit 1; }

cleanup() { echo 0 > "$ev"; echo > "$T/trace"; }
trap cleanup EXIT INT TERM

echo > "$T/trace"
echo 1 > "$ev"
echo "recording cross-CPU function calls for $secs s ..." >&2
sleep "$secs"
echo 0 > "$ev"

total=$(grep -c 'csd_queue_cpu:' "$T/trace")
printf '%d cross-CPU calls in %s s (%d/s)\n\n' "$total" "$secs" $(( total / secs ))
printf '%8s  %-28s %-40s %s\n' "count" "function" "called from" "task"
grep "csd_queue_cpu:" "$T/trace" \
	| sed -E 's/^ *(.+)-[0-9]+ +(\([ 0-9-]+\) +)?\[[0-9]+\].*callsite=([^ ]+) func=([^ ]+).*/\4 \3 \1/' \
	| sed -E 's/\+0x[0-9a-f]+\/0x[0-9a-f]+//g' \
	| sort | uniq -c | sort -rn | head -20 \
	| while read -r n func from task; do
		printf '%8d  %-28s %-40s %s\n' "$n" "$func" "$from" "$task"
	done
