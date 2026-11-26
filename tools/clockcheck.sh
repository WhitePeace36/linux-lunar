#!/usr/bin/env bash
# clockcheck: is the kernel timing with the TSC, and if not, why?
#
# Games ask for the time constantly (THE FINALS: ~100k NtQueryPerformanceCounter
# calls a second on its main thread). With the TSC that is a few ns in the
# vDSO; once the kernel has switched to the HPET, every call is a syscall and
# an HPET read (~1 us), and under Wine also a Syscall User Dispatch trap.
#
#   tools/clockcheck.sh          (sudo for the kernel log on most systems)

cs=/sys/devices/system/clocksource/clocksource0
cur=$(cat $cs/current_clocksource 2>/dev/null)
avail=$(cat $cs/available_clocksource 2>/dev/null)
flags=$(grep -m1 '^flags' /proc/cpuinfo)
has() { [[ " $flags " == *" $1 "* ]] && echo yes || echo no; }

echo "clocksource: ${cur:-?} (available: ${avail:-?})"
printf 'CPU: %s\n' "$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
printf 'TSC: constant %s, nonstop %s, TSC_ADJUST %s, known freq %s, hypervisor %s\n' \
	"$(has constant_tsc)" "$(has nonstop_tsc)" "$(has tsc_adjust)" "$(has tsc_known_freq)" "$(has hypervisor)"
grep -o '\(tsc\|clocksource\)=[^ ]*' /proc/cmdline | sed 's/^/boot parameter: /'

log=$(dmesg 2>/dev/null || journalctl -k -b --no-pager -q 2>/dev/null)
[[ -n "$log" ]] || echo "(kernel log not readable; run with sudo for the cause)"
events=$(grep -E "Marking clocksource 'tsc|Marking TSC unstable|TSC warp|TSC synchronization .*failed|Switched to clocksource|skewed .* over watchdog" <<<"$log")
[[ -n "$events" ]] && { echo "kernel log:"; sed 's/^/  /' <<<"$events" | tail -n 12; }

echo
if [[ "$cur" == tsc ]]; then
	echo "OK: timing with the TSC."
	vendor=$(grep -m1 '^vendor_id' /proc/cpuinfo | awk '{print $3}')
	[[ $(has tsc_adjust) == no && $(has constant_tsc) == yes && $(has nonstop_tsc) == yes &&
	   $(has hypervisor) == no && ( $vendor == AuthenticAMD || $vendor == HygonGenuine ) ]] &&
		echo "Note: no TSC_ADJUST (AMD before Zen 5), so unless this kernel has linux-lunar patch 0015" \
		     "the clocksource watchdog keeps checking the TSC and can still switch to the HPET later."
elif grep -q "check_tsc_sync_source failed\|TSC warp" <<<"$events"; then
	echo "The TSCs of different CPUs disagree (synchronization check at CPU bringup)."
	echo "That is a firmware/hardware problem, a BIOS update may fix it. Patch 0015 does not change this check."
elif grep -q "clocksource watchdog\|skew is too large\|skewed" <<<"$events"; then
	echo "The clocksource watchdog rejected the TSC (skew against HPET/ACPI PM timer)."
	echo "With an invariant TSC on one socket this is often a false alarm of the watchdog itself."
	echo "Fix: linux-lunar patch 0015 (_patch_tsc_zen_nowatchdog), or the boot parameter tsc=nowatchdog."
elif [[ -n "$cur" ]]; then
	echo "Not using the TSC (${cur}); the kernel log above may tell why."
fi
