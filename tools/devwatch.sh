#!/usr/bin/env bash
# devwatch.sh - find the device or driver that makes the whole system stutter
# Usage: sudo ./devwatch.sh [interval_s]      (read-only; Ctrl+C to stop)
#
# Every interval it shows what a misbehaving device typically causes:
#   PSI       time stolen by interrupts (irq), CPU/IO/memory waits (avg10, %)
#   IRQs      the busiest interrupt lines (per second, with their CPU spread)
#   DISK      busiest block devices: busy %, IO/s, average wait, requests in
#             flight; "stuck" when requests are in flight but none complete
#   WAIT      tasks blocked in D state (usually waiting for a device), with
#             where they wait in the kernel
#   IOWAIT    (only when io pressure >= 5 %) threads the kernel counts as
#             waiting for I/O although they are not in D state, e.g. programs
#             waiting on io_uring with requests pending: they raise PSI io and
#             CPU iowait without any disk activity
#   AER       new PCIe correctable/non-fatal/fatal errors per device
#   kthreads  kernel threads using the most CPU (kworker, irq/, ksoftirqd)
#   klog      new kernel error messages and USB disconnects
# Lines marked with ! exceed a threshold worth investigating.

iv="${1:-2}"
IRQ_WARN=20000     # interrupts/s on one line
DISK_BUSY_WARN=90  # % of the time a device had requests outstanding
DISK_AWAIT_WARN=500 # ms average per completed request
KTH_WARN=20        # % of one CPU for one kernel thread
ncpu=$(nproc)
HZ=$(getconf CLK_TCK)

(( EUID == 0 )) || echo "note: run with sudo to see kernel log and all AER counters"

psi() { # file -> "some_avg10 full_avg10"
	[[ -r "$1" ]] || { echo "- -"; return; }
	awk '/^some/ { split($2, a, "="); s = a[2] } /^full/ { split($2, a, "="); f = a[2] }
	     END { printf "%s %s", (s == "" ? "-" : s), (f == "" ? "-" : f) }' "$1"
}

irq_snapshot() { # "irq total name"
	awk -v n="$ncpu" 'NR > 1 { t = 0; for (i = 2; i <= n + 1; i++) t += $i
		name = ""; for (i = n + 2; i <= NF; i++) name = name " " $i
		sub(/:$/, "", $1); print $1, t, name }' /proc/interrupts
}

aer_snapshot() { # "dev kind total"
	local f dev kind
	for f in /sys/bus/pci/devices/*/aer_dev_{correctable,nonfatal,fatal}; do
		[[ -r "$f" ]] || continue
		dev="${f%/*}"; dev="${dev##*/}"; kind="${f##*_}"
		awk -v d="$dev" -v k="$kind" '/^TOTAL/ { print d, k, $2 }' "$f"
	done
}

kthread_snapshot() { # "pid ticks comm" for kernel threads (children of kthreadd)
	local s
	for s in /proc/[0-9]*/stat; do
		read -r line 2>/dev/null < "$s" || continue
		# comm may contain spaces: take what is between the outer parens
		local pid="${line%% *}" rest="${line##*) }" comm="${line#*(}"; comm="${comm%)*}"
		set -- $rest
		[[ "$2" == 2 ]] || continue            # ppid 2 = kthreadd child
		echo "$pid $(( ${12} + ${13} )) $comm"
	done
}

disk_snapshot() { # "dev ios wait_ms ticks_ms inflight" for whole block devices
	awk '{ if (system("test -e /sys/block/" $3) == 0 && $3 !~ /^(loop|ram)/)
		print $3, $4 + $8, $7 + $11, $13, $12 }' /proc/diskstats
}

disk_name() { # dev -> model / transport
	local d="$1" m=""
	[[ -r /sys/block/$d/device/model ]] && m=$(tr -s ' ' < "/sys/block/$d/device/model")
	[[ "$(readlink -f "/sys/block/$d" 2>/dev/null)" == */usb* ]] && m="$m (USB)"
	[[ $d == zram* ]] && m="zram swap"
	echo "${m:-?}"
}

dstate_tasks() { # "pid comm" of tasks in uninterruptible sleep
	local s line pid comm rest
	for s in /proc/[0-9]*/stat; do
		read -r line 2>/dev/null < "$s" || continue
		rest="${line##*) }"
		[[ "${rest:0:1}" == D ]] || continue
		pid="${line%% *}"; comm="${line#*(}"; comm="${comm%)*}"
		echo "$pid $comm"
	done
}

kstack() { # pid -> "frame <- frame <- frame" (needs root)
	[[ -r /proc/$1/stack ]] || { cat "/proc/$1/wchan" 2>/dev/null; return; }
	awk '{ sub(/^\[<[0-9a-f]+>\] /, ""); sub(/\+0x.*/, ""); f[NR] = $0 }
	     END { for (i = 1; i <= NR && i <= 4; i++) printf "%s%s", (i > 1 ? " <- " : ""), f[i] }' \
		"/proc/$1/stack" 2>/dev/null
}

iowait_threads() { # "pid tid state frame" of non-D threads sleeping in an I/O wait
	local f tid pid st
	grep -l -E 'io_cqring_wait|io_schedule' /proc/[0-9]*/task/[0-9]*/stack 2>/dev/null | while read -r f; do
		tid="${f%/stack}"; tid="${tid##*/}"; pid="${f#/proc/}"; pid="${pid%%/*}"
		st=$(awk '/^State/ { print $2 }' "/proc/$pid/task/$tid/status" 2>/dev/null)
		[[ -n "$st" && "$st" != D ]] || continue
		echo "$pid $tid $st $(grep -o -m1 -E 'io_cqring_wait|io_schedule[a-z_]*' "$f" 2>/dev/null)"
	done
}

klog_counts() { # "errors usb_disconnects"
	if (( EUID == 0 )); then
		local e u
		e=$(dmesg --level=emerg,alert,crit,err 2>/dev/null | wc -l)
		u=$(dmesg 2>/dev/null | grep -c "USB disconnect")
		echo "$e $u"
	else
		echo "- -"
	fi
}

declare -A irq0 aer0 kth0 dio0 dwt0 dtk0
while read -r i t _; do irq0[$i]=$t; done < <(irq_snapshot)
while read -r d n w k _; do dio0[$d]=$n; dwt0[$d]=$w; dtk0[$d]=$k; done < <(disk_snapshot)
while read -r d k t; do aer0[$d.$k]=$t; done < <(aer_snapshot)
while read -r p t _; do kth0[$p]=$t; done < <(kthread_snapshot)
read -r err0 usb0 <<<"$(klog_counts)"

while sleep "$iv"; do
	echo "=== $(date +%T)"
	read -r _ irqf <<<"$(psi /proc/pressure/irq)"
	read -r cpus _ <<<"$(psi /proc/pressure/cpu)"
	read -r ios iof <<<"$(psi /proc/pressure/io)"
	read -r mems memf <<<"$(psi /proc/pressure/memory)"
	printf 'PSI      irq %s%%  cpu %s%%  io %s/%s%%  mem %s/%s%%%s\n' "$irqf" "$cpus" "$ios" "$iof" "$mems" "$memf" \
		"$([[ -r /proc/pressure/irq ]] || echo '  (irq: kernel lacks CONFIG_IRQ_TIME_ACCOUNTING)')"

	# busiest interrupt lines
	out=()
	while read -r i t name; do
		d=$(( (t - ${irq0[$i]:-$t}) / iv )); irq0[$i]=$t
		(( d > 0 )) && out+=("$d $i $name")
	done < <(irq_snapshot)
	printf '%s\n' "${out[@]}" | sort -rn | head -4 | while read -r d i name; do
		mark=" "; (( d >= IRQ_WARN )) && mark="!"
		printf 'IRQ %s %8d/s  %-5s %s\n' "$mark" "$d" "$i" "$name"
	done

	# block devices
	out=()
	while read -r d n w k inf; do
		dn=$(( n - ${dio0[$d]:-$n} )); dw=$(( w - ${dwt0[$d]:-$w} )); dk=$(( k - ${dtk0[$d]:-$k} ))
		dio0[$d]=$n; dwt0[$d]=$w; dtk0[$d]=$k
		busy=$(( dk * 100 / (iv * 1000) )); (( busy > 100 )) && busy=100
		(( busy > 0 || inf > 0 )) || continue
		await=0; (( dn > 0 )) && await=$(( dw / dn ))
		out+=("$busy $d $(( dn / iv )) $await $inf")
	done < <(disk_snapshot)
	printf '%s\n' "${out[@]}" | sort -rn | head -3 | while read -r busy d iops await inf; do
		[[ -z "$busy" ]] && continue
		mark=" "; note=""
		(( busy >= DISK_BUSY_WARN || await >= DISK_AWAIT_WARN )) && mark="!"
		(( inf > 0 && iops == 0 )) && { mark="!"; note="  STUCK: requests in flight, none completed"; }
		printf 'DISK %s %-9s %3d%% busy %6d IO/s  await %5d ms  in flight %d  (%s)%s\n' \
			"$mark" "$d" "$busy" "$iops" "$await" "$inf" "$(disk_name "$d")" "$note"
	done

	# tasks blocked in D state
	n=0
	while read -r p comm; do
		[[ -z "$p" ]] && continue
		n=$(( n + 1 )); (( n > 5 )) && continue
		printf 'WAIT !  %-7s %-16s %s\n' "$p" "$comm" "$(kstack "$p")"
	done < <(dstate_tasks)
	(( n > 5 )) && printf 'WAIT !  ... %d more tasks in D state\n' $(( n - 5 ))

	# I/O wait without D state (only worth the scan when there is io pressure)
	if (( EUID == 0 )) && [[ "$ios" != - ]] && awk -v v="$ios" 'BEGIN { exit !(v >= 5) }'; then
		n=0
		while read -r p t st fr; do
			[[ -z "$p" ]] && continue
			n=$(( n + 1 )); (( n > 5 )) && continue
			what="$fr"
			[[ "$fr" == io_cqring_wait ]] && what="io_uring wait with requests pending (counted as I/O wait, not disk I/O)"
			printf 'IOWAIT  %-7s %-7s %s %-16s %s\n' "$p" "$t" "$st" "$(cat "/proc/$p/task/$t/comm" 2>/dev/null)" "$what"
		done < <(iowait_threads)
		(( n > 5 )) && printf 'IOWAIT  ... %d more threads\n' $(( n - 5 ))
	fi

	# PCIe AER
	while read -r dv k t; do
		key="$dv.$k"; d=$(( t - ${aer0[$key]:-$t} )); aer0[$key]=$t
		(( d > 0 )) && printf 'AER !  %s %s +%d  (%s)\n' "$dv" "$k" "$d" \
			"$(lspci -s "$dv" 2>/dev/null | cut -d' ' -f2- | cut -c1-60)"
	done < <(aer_snapshot)

	# kernel threads by CPU
	out=()
	declare -A seen=()
	while read -r p t comm; do
		seen[$p]=1
		d=$(( t - ${kth0[$p]:-$t} )); kth0[$p]=$t
		pct=$(( d * 100 / (HZ * iv) ))
		(( pct > 0 )) && out+=("$pct $p $comm")
	done < <(kthread_snapshot)
	for p in "${!kth0[@]}"; do [[ -n "${seen[$p]}" ]] || unset "kth0[$p]"; done
	printf '%s\n' "${out[@]}" | sort -rn | head -3 | while read -r pct p comm; do
		[[ -z "$pct" ]] && continue
		mark=" "; (( pct >= KTH_WARN )) && mark="!"
		printf 'KTHR %s %4d%%  %-7s %s\n' "$mark" "$pct" "$p" "$comm"
	done

	# kernel log
	read -r err1 usb1 <<<"$(klog_counts)"
	if [[ "$err1" != - ]]; then
		de=$(( err1 - err0 )); du=$(( usb1 - usb0 )); err0=$err1; usb0=$usb1
		(( de > 0 || du > 0 )) && printf 'KLOG !  +%d errors, +%d USB disconnects   (dmesg --level=err | tail)\n' "$de" "$du"
	fi
done
