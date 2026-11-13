#!/usr/bin/env bash
# memcheck.sh - show the memory-stall related settings of the running kernel
# Usage: ./memcheck.sh        (no root needed)

val() { cat "$1" 2>/dev/null || echo "n/a"; }
sel() { grep -o '\[[^]]*\]' "$1" 2>/dev/null | tr -d '[]' || echo "n/a"; }

echo "== kernel"
uname -r
sed 's/.*#1/#1/' /proc/version

echo
echo "== patch 4: soft defrag mode (defrag_mode 1 + defrag_soft 1; defrag_soft needs patch 4)"
printf '  %-32s %s\n' vm.defrag_mode "$(sysctl -n vm.defrag_mode 2>/dev/null)"
printf '  %-32s %s\n' vm.defrag_soft "$(sysctl -n vm.defrag_soft 2>/dev/null || echo 'n/a (patch 4 missing)')"

echo
echo "== patch 3: TTM GPU allocations (0 = upstream, 1 = one bounded round, 2 = no reclaim)"
printf '  %-32s %s\n' ttm.low_latency_alloc \
	"$(val /sys/module/ttm/parameters/low_latency_alloc)"

echo
echo "== patch 6: bounded LRU drains (needs patched kernel)"
printf '  %-32s %s\n' vm.lru_drain_timeout_ms "$(sysctl -n vm.lru_drain_timeout_ms 2>/dev/null || echo n/a)"
for k in lru_drain_timeout lru_drain_slow; do
	printf '  %-32s %s\n' "$k (since boot)" "$(awk -v k=$k '$1 == k { print $2; f = 1 } END { if (!f) print "n/a" }' /proc/vmstat)"
done

echo
echo "== patch 8: direct reclaim splits huge pages instead of compressing them whole (needs patched kernel)"
printf '  %-32s %s\n' vm.thp_direct_reclaim_split "$(sysctl -n vm.thp_direct_reclaim_split 2>/dev/null || echo n/a)"
printf '  %-32s %s\n' vm.lru_gen_defer_direct_aging "$(sysctl -n vm.lru_gen_defer_direct_aging 2>/dev/null || echo n/a)"
for k in thp_swpout_direct_split thp_swpout thp_swpout_fallback; do
	printf '  %-32s %s\n' "$k (since boot)" "$(awk -v k=$k '$1 == k { print $2; f = 1 } END { if (!f) print "n/a" }' /proc/vmstat)"
done

echo
echo "== watermarks / reclaim / compaction"
for k in watermark_scale_factor watermark_boost_factor compaction_proactiveness \
	 extfrag_threshold min_free_kbytes swappiness page-cluster vfs_cache_pressure; do
	printf '  %-32s %s\n' "vm.$k" "$(sysctl -n vm.$k 2>/dev/null)"
done

echo
echo "== writeback / misc"
for k in vm.dirty_bytes vm.dirty_background_bytes vm.dirty_ratio vm.dirty_background_ratio \
	 vm.dirty_writeback_centisecs vm.dirty_expire_centisecs vm.max_map_count kernel.split_lock_mitigate; do
	printf '  %-32s %s\n' "$k" "$(sysctl -n $k 2>/dev/null || echo n/a)"
done
printf '  %-32s %s\n' split_lock_detect \
	"$(journalctl -k -b -o cat 2>/dev/null | grep -m1 -o 'x86/split lock detection: #.*' || echo 'off, or not supported by this CPU')"

echo
echo "== THP"
t=/sys/kernel/mm/transparent_hugepage
printf '  %-32s %s\n' enabled "$(sel $t/enabled)" defrag "$(sel $t/defrag)" \
	shmem_enabled "$(sel $t/shmem_enabled)" \
	khugepaged/defrag "$(val $t/khugepaged/defrag)" \
	khugepaged/pages_to_scan "$(val $t/khugepaged/pages_to_scan)" \
	khugepaged/scan_sleep_millisecs "$(val $t/khugepaged/scan_sleep_millisecs)" \
	khugepaged/alloc_sleep_millisecs "$(val $t/khugepaged/alloc_sleep_millisecs)" \
	khugepaged/max_ptes_none "$(val $t/khugepaged/max_ptes_none)" \
	khugepaged/max_ptes_swap "$(val $t/khugepaged/max_ptes_swap)"

echo
echo "== MGLRU"
printf '  %-32s %s\n' lru_gen/enabled "$(val /sys/kernel/mm/lru_gen/enabled)" \
	lru_gen/min_ttl_ms "$(val /sys/kernel/mm/lru_gen/min_ttl_ms)"

echo
echo "== zram"
for d in /sys/block/zram*; do
	[ -e "$d" ] || continue
	printf '  %-32s %s, %s\n' "${d##*/}" \
		"$(numfmt --to=iec "$(val "$d/disksize")" 2>/dev/null)" \
		"$(sel "$d/comp_algorithm")"
done

echo
echo "== live state per zone (patch 5 / Zen revert: node_unreclaimable should be 0)"
awk '/^Node/ { z = $4 }
     /node_unreclaimable/ { u[z] = $2 }
     /^ +boost/ { b[z] = $2 }
     /^ +min / { m[z] = $2 } /^ +low / { l[z] = $2 } /^ +high / { h[z] = $2 }
     END { printf "  %-8s %8s %10s %10s %10s %10s\n", "zone", "unrecl", "boost MB", "min MB", "low MB", "high MB";
	   for (z in u) printf "  %-8s %8s %10.0f %10.0f %10.0f %10.0f\n", z, u[z],
		b[z]*4/1024, m[z]*4/1024, l[z]*4/1024, h[z]*4/1024 }' /proc/zoneinfo
