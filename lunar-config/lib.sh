# shellcheck shell=bash
# linux-lunar shared functions.
# Sourced by lunar.sh (resolve options, write .lunar-build.cfg) and by the
# PKGBUILD's prepare() (patch, set defaults, configure).

# ---------------------------------------------------------------- messages --

_l_msg()  { printf '\033[1;34m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
_l_msg2() { printf '  \033[1;34m->\033[0m %s\n' "$*"; }
_l_warn() { printf '\033[1;33m==> WARNING:\033[0m %s\n' "$*" >&2; }
_l_die()  { printf '\033[1;31m==> ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

_l_true() { [[ "${1,,}" == "true" || "${1,,}" == "yes" || "$1" == "1" ]]; }

# ------------------------------------------------------------- the options --

# Every option customization.cfg knows; written to .lunar-build.cfg as resolved.
_LUNAR_OPTIONS=(
	_version _pkgsuffix _localversion _pgp_verify
	_base_config _localmodconfig _modprobeddb_path _menuconfig
	_compiler _lto_mode _march _kcflags _jobs
	_timer_freq _preempt _preempt_dynamic _tickless _nr_cpus _cpu_governor
	_thp_enabled _thp_defrag _thp_shmem_enabled _thp_khugepaged_defrag
	_thp_khugepaged_alloc_sleep_ms _thp_khugepaged_pages_to_scan
	_thp_khugepaged_scan_sleep_ms _thp_khugepaged_max_ptes_swap _mglru _mglru_min_ttl_ms
	_patch_filemap_large_folios _patch_boost_min_wmark _patch_ttm_low_latency
	_patch_soft_defrag_mode _patch_kswapd_hopeless _patch_lru_drain_bounded
	_patch_khugepaged_prio _patch_direct_reclaim_split
	_default_defrag_mode _default_defrag_soft _default_watermark_scale_factor
	_default_watermark_boost_factor _default_compaction_proactiveness
	_default_swappiness _default_ttm_low_latency _default_lru_drain_timeout_ms
	_default_thp_direct_reclaim_split
	_default_dirty_bytes _default_dirty_background_bytes _default_dirty_writeback_centisecs
	_default_dirty_expire_centisecs
	_default_max_map_count _default_split_lock_detect _default_split_lock_mitigate
	_user_patches _user_patches_confirm _build_headers _noninteractive
	_cleanup_on_abort _cleanup_after_build _keep_kernel_source
)

# Load customization.cfg, then the personal override file.
lunar_load_options() {
	local where="$1"
	# shellcheck disable=SC1091
	source "$where/customization.cfg"
	local user_cfg="${XDG_CONFIG_HOME:-$HOME/.config}/linux-lunar.cfg"
	if [[ -f "$user_cfg" ]]; then
		_l_msg2 "Reading overrides from $user_cfg"
		# shellcheck disable=SC1090
		source "$user_cfg"
	fi
}

_l_in() { local x="$1"; shift; local y; for y; do [[ "$x" == "$y" ]] && return 0; done; return 1; }
_l_int_range() { [[ "$1" =~ ^[0-9]+$ ]] && (( $1 >= $2 && $1 <= $3 )); }

# Check every option; die on the first invalid one.
lunar_validate_options() {
	_lto_mode="${_lto_mode:-none}"          # empty means no LTO
	_cpu_governor="${_cpu_governor:-performance}"
	_l_in "$_compiler" gcc llvm || _l_die "_compiler must be gcc or llvm (is '$_compiler')"
	_l_in "$_lto_mode" none thin full || _l_die "_lto_mode must be none, thin or full"
	if [[ "$_lto_mode" != none && "$_compiler" != llvm ]]; then
		_l_die "_lto_mode=$_lto_mode needs _compiler=llvm"
	fi
	_l_in "$_timer_freq" 100 250 300 1000 || _l_die "_timer_freq must be 100, 250, 300 or 1000"
	_l_in "$_preempt" lazy full rt || _l_die "_preempt must be lazy, full or rt"
	_l_in "$_tickless" periodic idle full || _l_die "_tickless must be periodic, idle or full"
	_l_in "$_cpu_governor" performance schedutil powersave \
		|| _l_die "_cpu_governor must be performance, schedutil or powersave"
	_l_in "$_menuconfig" none menuconfig nconfig xconfig || _l_die "_menuconfig must be none, menuconfig, nconfig or xconfig"
	_l_in "$_thp_enabled" always madvise never || _l_die "_thp_enabled must be always, madvise or never"
	_l_in "$_thp_defrag" always defer defer+madvise madvise never || _l_die "_thp_defrag must be always, defer, defer+madvise, madvise or never"
	_l_in "${_thp_khugepaged_defrag,,}" true false yes no 1 0 || _l_die "_thp_khugepaged_defrag must be true or false"
	[[ -z "$_thp_khugepaged_alloc_sleep_ms" ]] || _l_int_range "$_thp_khugepaged_alloc_sleep_ms" 1000 3600000 \
		|| _l_die "_thp_khugepaged_alloc_sleep_ms must be 1000..3600000 (or empty)"
	[[ -z "$_thp_shmem_enabled" ]] || _l_in "$_thp_shmem_enabled" always within_size advise never \
		|| _l_die "_thp_shmem_enabled must be always, within_size, advise or never (or empty)"
	[[ -z "$_thp_khugepaged_pages_to_scan" ]] || _l_int_range "$_thp_khugepaged_pages_to_scan" 1 4194304 \
		|| _l_die "_thp_khugepaged_pages_to_scan must be 1..4194304 (or empty)"
	[[ -z "$_thp_khugepaged_scan_sleep_ms" ]] || _l_int_range "$_thp_khugepaged_scan_sleep_ms" 100 3600000 \
		|| _l_die "_thp_khugepaged_scan_sleep_ms must be 100..3600000 (or empty)"
	[[ -z "$_thp_khugepaged_max_ptes_swap" ]] || _l_int_range "$_thp_khugepaged_max_ptes_swap" 0 511 \
		|| _l_die "_thp_khugepaged_max_ptes_swap must be 0..511 (or empty)"
	[[ -z "$_mglru_min_ttl_ms" ]] || _l_int_range "$_mglru_min_ttl_ms" 0 600000 \
		|| _l_die "_mglru_min_ttl_ms must be 0..600000 (or empty)"
	[[ -z "$_nr_cpus" ]] || _l_int_range "$_nr_cpus" 2 8192 || _l_die "_nr_cpus must be a number from 2 to 8192"
	[[ -z "$_jobs" ]] || _l_int_range "$_jobs" 1 4096 || _l_die "_jobs must be a positive number"
	[[ "$_march" =~ ^[A-Za-z0-9_.+-]+$ ]] || _l_die "_march contains invalid characters"
	[[ "$_pkgsuffix" =~ ^[a-z0-9][a-z0-9_.+-]*$ ]] || _l_die "_pkgsuffix must be lower case letters, digits and _.+-"
	[[ -z "$_localversion" || "$_localversion" =~ ^[A-Za-z0-9_.+-]+$ ]] || _l_die "_localversion contains invalid characters"

	if [[ "$_default_defrag_mode" == 2 ]]; then   # soft mode used to be defrag_mode=2
		_l_warn "_default_defrag_mode=2 is now _default_defrag_mode=1 plus _default_defrag_soft=true; using that"
		_default_defrag_mode=1
		_default_defrag_soft=true
	fi
	[[ -z "$_default_defrag_mode" ]] || _l_int_range "$_default_defrag_mode" 0 1 \
		|| _l_die "_default_defrag_mode must be 0 or 1 (or empty)"
	if [[ -n "$_default_defrag_soft" ]]; then
		_l_in "${_default_defrag_soft,,}" true false yes no 1 0 || _l_die "_default_defrag_soft must be true or false"
		if _l_true "$_default_defrag_soft"; then
			_l_true "$_patch_soft_defrag_mode" || _l_die "_default_defrag_soft=true needs _patch_soft_defrag_mode=true"
			[[ "$_default_defrag_mode" == 1 ]] \
				|| _l_warn "_default_defrag_soft=true has no effect unless vm.defrag_mode is 1"
		fi
	fi
	[[ -z "$_default_watermark_scale_factor" ]] || _l_int_range "$_default_watermark_scale_factor" 1 3000 \
		|| _l_die "_default_watermark_scale_factor must be 1..3000"
	[[ -z "$_default_watermark_boost_factor" ]] || _l_int_range "$_default_watermark_boost_factor" 0 1000000 \
		|| _l_die "_default_watermark_boost_factor must be a number >= 0"
	[[ -z "$_default_compaction_proactiveness" ]] || _l_int_range "$_default_compaction_proactiveness" 0 100 \
		|| _l_die "_default_compaction_proactiveness must be 0..100"
	[[ -z "$_default_swappiness" ]] || _l_int_range "$_default_swappiness" 0 200 \
		|| _l_die "_default_swappiness must be 0..200"
	case "${_default_ttm_low_latency,,}" in   # true/false from older configs
		true|yes)  _default_ttm_low_latency=1 ;;
		false|no)  _default_ttm_low_latency=0 ;;
	esac
	if [[ -n "$_default_ttm_low_latency" ]]; then
		_l_int_range "$_default_ttm_low_latency" 0 2 \
			|| _l_die "_default_ttm_low_latency must be 0, 1 or 2 (or empty)"
		if (( _default_ttm_low_latency > 0 )) && ! _l_true "$_patch_ttm_low_latency"; then
			_l_die "_default_ttm_low_latency=$_default_ttm_low_latency needs _patch_ttm_low_latency=true"
		fi
	fi
	if [[ -n "$_default_lru_drain_timeout_ms" ]]; then
		_l_int_range "$_default_lru_drain_timeout_ms" 0 1000 \
			|| _l_die "_default_lru_drain_timeout_ms must be 0..1000"
		_l_true "$_patch_lru_drain_bounded" \
			|| _l_die "_default_lru_drain_timeout_ms needs _patch_lru_drain_bounded=true (or leave it empty)"
	fi
	if [[ -n "$_default_thp_direct_reclaim_split" ]]; then
		_l_in "${_default_thp_direct_reclaim_split,,}" true false yes no 1 0 \
			|| _l_die "_default_thp_direct_reclaim_split must be true or false"
		if _l_true "$_default_thp_direct_reclaim_split"; then
			_l_true "$_patch_direct_reclaim_split" \
				|| _l_die "_default_thp_direct_reclaim_split=true needs _patch_direct_reclaim_split=true"
		fi
	fi
	[[ -z "$_default_dirty_bytes" ]] || _l_int_range "$_default_dirty_bytes" 8192 1099511627776 \
		|| _l_die "_default_dirty_bytes must be 8192..1099511627776 (or empty)"
	[[ -z "$_default_dirty_background_bytes" ]] || _l_int_range "$_default_dirty_background_bytes" 1 1099511627776 \
		|| _l_die "_default_dirty_background_bytes must be 1..1099511627776 (or empty)"
	if [[ -n "$_default_dirty_bytes" && -n "$_default_dirty_background_bytes" ]] \
	   && (( _default_dirty_background_bytes >= _default_dirty_bytes )); then
		_l_warn "_default_dirty_background_bytes >= _default_dirty_bytes: the kernel then uses half of dirty_bytes"
	fi
	[[ -z "$_default_dirty_writeback_centisecs" ]] || _l_int_range "$_default_dirty_writeback_centisecs" 0 100000 \
		|| _l_die "_default_dirty_writeback_centisecs must be 0..100000 (or empty)"
	[[ -z "$_default_dirty_expire_centisecs" ]] || _l_int_range "$_default_dirty_expire_centisecs" 0 100000 \
		|| _l_die "_default_dirty_expire_centisecs must be 0..100000 (or empty)"
	[[ -z "$_default_max_map_count" ]] || _l_int_range "$_default_max_map_count" 65530 2147483647 \
		|| _l_die "_default_max_map_count must be 65530..2147483647 (or empty)"
	[[ -z "$_default_split_lock_detect" ]] || _l_in "$_default_split_lock_detect" off warn fatal \
		|| _l_die "_default_split_lock_detect must be off, warn or fatal (or empty)"
	[[ -z "$_default_split_lock_mitigate" ]] || _l_int_range "$_default_split_lock_mitigate" 0 1 \
		|| _l_die "_default_split_lock_mitigate must be 0 or 1 (or empty)"
	return 0
}

# ------------------------------------------------------------ versions -----

# "7.3.1" -> "7.3", "7.4-rc1" -> "7.4", "7.3" -> "7.3"
lunar_series_of() { local v="${1%%-rc*}"; echo "${v%.*}" | grep -Eo '^[0-9]+\.[0-9]+' || echo "$v"; }

# Patch series linux-lunar ships, e.g. "7.3 7.4"
lunar_supported_series() {
	local where="$1" d out=()
	for d in "$where"/lunar-patches/*/; do
		[[ -f "$d/series" ]] && out+=("$(basename "$d")")
	done
	printf '%s\n' "${out[@]}" | sort -V
}

# kernel.org releases.json (cached for one hour)
_lunar_releases_json() {
	local cache="$1/.lunar-releases.json"
	if [[ ! -s "$cache" || -n "$(find "$cache" -mmin +60 2>/dev/null)" ]]; then
		if curl -fsSL --retry 2 https://www.kernel.org/releases.json -o "$cache.tmp"; then
			mv "$cache.tmp" "$cache"
		else
			rm -f "$cache.tmp"
			[[ -s "$cache" ]] || return 1
			_l_warn "kernel.org unreachable, using cached release list"
		fi
	fi
	cat "$cache"
}

# Print "version moniker" for every current kernel.org release.
lunar_current_releases() {
	_lunar_releases_json "$1" | python3 -c '
import json, sys
for r in json.load(sys.stdin)["releases"]:
    if r["moniker"] != "linux-next":
        print(r["version"], r["moniker"])'
}

# Resolve "7.3" to the newest 7.3.x (or 7.3-rcN while 7.3 is in development).
lunar_resolve_series() {
	local where="$1" series="$2" v best=""
	while read -r v _; do
		[[ "$v" == "$series" || "$v" == "$series".* || "$v" == "$series"-rc* ]] || continue
		best="$v"
	done < <(lunar_current_releases "$where" | sort -V)
	[[ -n "$best" ]] || return 1
	echo "$best"
}

# Set _kver, _kseries, _srcname, _src_url, _sig_url from a concrete version.
lunar_version_urls() {
	local v="$1" major
	_kseries="$(lunar_series_of "$v")"
	major="${_kseries%%.*}"
	if [[ "$v" =~ ^[0-9]+\.[0-9]+-rc[0-9]+$ ]]; then
		_kver="$v"
		_srcname="linux-$v"
		_src_url="https://git.kernel.org/torvalds/t/linux-$v.tar.gz"
		_sig_url=""
	elif [[ "$v" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
		[[ "$v" =~ \.0$ ]] && v="${v%.0}"
		_kver="$v"
		_srcname="linux-$v"
		_src_url="https://cdn.kernel.org/pub/linux/kernel/v$major.x/linux-$v.tar.xz"
		_sig_url="https://cdn.kernel.org/pub/linux/kernel/v$major.x/linux-$v.tar.sign"
	else
		_l_die "Cannot understand kernel version '$1' (use e.g. 7.3, 7.3.1 or 7.4-rc1)"
	fi
}

# ------------------------------------------------------------ patch lists --

# Fill _lunar_patches with the enabled patches of the series (relative paths).
lunar_collect_patches() {
	local where="$1" series="$2" var file
	_lunar_patches=()
	[[ -f "$where/lunar-patches/$series/series" ]] || return 0
	while read -r var file; do
		[[ -z "$var" || "$var" == \#* ]] && continue
		[[ -f "$where/lunar-patches/$series/$file" ]] \
			|| _l_die "lunar-patches/$series/series names missing file $file"
		if _l_true "${!var}"; then
			_lunar_patches+=("lunar-patches/$series/$file")
		fi
	done < "$where/lunar-patches/$series/series"
}

# Fill _lunar_userreverts / _lunar_userpatches (relative paths).
lunar_collect_userpatches() {
	local where="$1" series="$2" dir f
	dir="linux${series//./}-lunar-userpatches"
	_lunar_userreverts=() _lunar_userpatches=()
	_l_true "$_user_patches" || return 0
	[[ -d "$where/$dir" ]] || return 0
	for f in "$where/$dir"/*.myrevert; do [[ -e "$f" ]] && _lunar_userreverts+=("$dir/${f##*/}"); done
	for f in "$where/$dir"/*.mypatch;  do [[ -e "$f" ]] && _lunar_userpatches+=("$dir/${f##*/}"); done
	for f in "$where/$dir"/*.patch;    do [[ -e "$f" ]] && _l_warn "Ignoring $dir/${f##*/}: user patches must end in .mypatch or .myrevert"; done
	return 0
}

# ------------------------------------------------------- prepare() steps ----

_l_patch() { # file [-R]
	local f="$1" rev="${2:-}"
	_l_msg2 "${rev:+Reverting}${rev:-Applying} ${f##*/}"
	patch -Np1 ${rev:+-R} --no-backup-if-mismatch --fuzz="${_lunar_fuzz:-0}" -i "$f" \
		|| _l_die "${f##*/} does not apply to $_srcname. Nothing was built."
}

lunar_apply_patches() {
	local where="$1" f
	if (( ${#_lunar_userreverts[@]} )); then
		_l_msg "Reverting user patches"
		for f in "${_lunar_userreverts[@]}"; do _lunar_fuzz=2 _l_patch "$where/$f" -R; done
	fi
	if (( ${#_lunar_patches[@]} )); then
		_l_msg "Applying lunar patches"
		for f in "${_lunar_patches[@]}"; do _l_patch "$where/$f"; done
	else
		_l_warn "No lunar patches enabled"
	fi
	if (( ${#_lunar_userpatches[@]} )); then
		_l_msg "Applying user patches"
		for f in "${_lunar_userpatches[@]}"; do _lunar_fuzz=2 _l_patch "$where/$f"; done
	fi
}

# Replace exactly one source line matching regex $2 in file $1 with $3
# (\t in $3 becomes a tab). Dies unless exactly one line matches.
_l_set_line() {
	python3 - "$@" <<'PY' || _l_die "Cannot set default in $1 (pattern: $2)"
import re, sys
path, pattern, new = sys.argv[1], sys.argv[2], sys.argv[3].replace("\\t", "\t")
src = open(path).read()
rx = re.compile(pattern, re.M)
n = len(rx.findall(src))
if n != 1:
    sys.exit(f"  expected exactly one line matching, found {n}")
open(path, "w").write(rx.sub(lambda m: new, src))
PY
}

# Compile the chosen defaults into the source. Every value stays tunable.
lunar_set_defaults() {
	_l_msg "Setting compiled-in defaults"
	if [[ -n "$_default_defrag_mode" ]]; then
		_l_msg2 "vm.defrag_mode = $_default_defrag_mode"
		_l_set_line mm/page_alloc.c '^int defrag_mode( = [0-9]+)?;$' "int defrag_mode = $_default_defrag_mode;"
	fi
	if [[ -n "$_default_defrag_soft" ]] && _l_true "$_default_defrag_soft"; then
		_l_msg2 "vm.defrag_soft = 1"
		_l_set_line mm/page_alloc.c '^static int defrag_soft;$' 'static int defrag_soft = 1;'
	fi
	if [[ -n "$_default_watermark_scale_factor" ]]; then
		_l_msg2 "vm.watermark_scale_factor = $_default_watermark_scale_factor"
		_l_set_line mm/page_alloc.c '^static int watermark_scale_factor = [0-9]+;$' \
			"static int watermark_scale_factor = $_default_watermark_scale_factor;"
	fi
	if [[ -n "$_default_watermark_boost_factor" ]]; then
		_l_msg2 "vm.watermark_boost_factor = $_default_watermark_boost_factor"
		_l_set_line mm/page_alloc.c '^static int watermark_boost_factor __read_mostly = [0-9]+;$' \
			"static int watermark_boost_factor __read_mostly = $_default_watermark_boost_factor;"
	fi
	if [[ -n "$_default_compaction_proactiveness" ]]; then
		_l_msg2 "vm.compaction_proactiveness = $_default_compaction_proactiveness"
		_l_set_line mm/compaction.c '^static unsigned int __read_mostly sysctl_compaction_proactiveness = [0-9]+;$' \
			"static unsigned int __read_mostly sysctl_compaction_proactiveness = $_default_compaction_proactiveness;"
	fi
	if [[ -n "$_default_swappiness" ]]; then
		_l_msg2 "vm.swappiness = $_default_swappiness"
		_l_set_line mm/vmscan.c '^int vm_swappiness = [0-9]+;$' "int vm_swappiness = $_default_swappiness;"
	fi
	if [[ -n "$_default_ttm_low_latency" ]] && (( _default_ttm_low_latency > 0 )); then
		_l_msg2 "ttm.low_latency_alloc = $_default_ttm_low_latency"
		_l_set_line drivers/gpu/drm/ttm/ttm_pool.c '^static unsigned int ttm_pool_low_latency;$' \
			"static unsigned int ttm_pool_low_latency = $_default_ttm_low_latency;"
	fi
	if [[ -n "$_default_lru_drain_timeout_ms" ]]; then
		_l_msg2 "vm.lru_drain_timeout_ms = $_default_lru_drain_timeout_ms"
		_l_set_line mm/folio.c '^static unsigned int sysctl_lru_drain_timeout_ms __read_mostly = [0-9]+;$' \
			"static unsigned int sysctl_lru_drain_timeout_ms __read_mostly = $_default_lru_drain_timeout_ms;"
	fi
	if [[ -n "$_default_thp_direct_reclaim_split" ]] && _l_true "$_default_thp_direct_reclaim_split"; then
		_l_msg2 "vm.thp_direct_reclaim_split = 1"
		_l_set_line mm/vmscan.c '^static int sysctl_thp_direct_reclaim_split __read_mostly;$' \
			'static int sysctl_thp_direct_reclaim_split __read_mostly = 1;'
	fi
	# Like writing vm.dirty_bytes / vm.dirty_background_bytes at runtime:
	# the matching *_ratio becomes 0, so sysctl shows which one is in use.
	if [[ -n "$_default_dirty_bytes" ]]; then
		_l_msg2 "vm.dirty_bytes = $_default_dirty_bytes"
		_l_set_line mm/page-writeback.c '^static unsigned long vm_dirty_bytes;$' \
			"static unsigned long vm_dirty_bytes = ${_default_dirty_bytes}UL;"
		_l_set_line mm/page-writeback.c '^static int vm_dirty_ratio = [0-9]+;$' 'static int vm_dirty_ratio;'
	fi
	if [[ -n "$_default_dirty_background_bytes" ]]; then
		_l_msg2 "vm.dirty_background_bytes = $_default_dirty_background_bytes"
		_l_set_line mm/page-writeback.c '^static unsigned long dirty_background_bytes;$' \
			"static unsigned long dirty_background_bytes = ${_default_dirty_background_bytes}UL;"
		_l_set_line mm/page-writeback.c '^static int dirty_background_ratio = [0-9]+;$' 'static int dirty_background_ratio;'
	fi
	if [[ -n "$_default_dirty_writeback_centisecs" ]]; then
		_l_msg2 "vm.dirty_writeback_centisecs = $_default_dirty_writeback_centisecs"
		_l_set_line mm/page-writeback.c '^unsigned int dirty_writeback_interval = [0-9]+ \* 100;.*$' \
			"unsigned int dirty_writeback_interval = $_default_dirty_writeback_centisecs; /* centiseconds */"
	fi
	if [[ -n "$_default_dirty_expire_centisecs" ]]; then
		_l_msg2 "vm.dirty_expire_centisecs = $_default_dirty_expire_centisecs"
		_l_set_line mm/page-writeback.c '^unsigned int dirty_expire_interval = [0-9]+ \* 100;.*$' \
			"unsigned int dirty_expire_interval = $_default_dirty_expire_centisecs; /* centiseconds */"
	fi
	if [[ -n "$_default_max_map_count" ]]; then
		_l_msg2 "vm.max_map_count = $_default_max_map_count"
		_l_set_line mm/util.c '^int sysctl_max_map_count __read_mostly = [A-Z_0-9]+;$' \
			"int sysctl_max_map_count __read_mostly = $_default_max_map_count;"
	fi
	if [[ -n "$_default_split_lock_detect" ]]; then
		_l_msg2 "split_lock_detect = $_default_split_lock_detect (kernel command line still overrides)"
		_l_set_line arch/x86/kernel/cpu/bus_lock.c '^\tenum split_lock_detect_state state = sld_[a-z]+;$' \
			"\tenum split_lock_detect_state state = sld_$_default_split_lock_detect;"
	fi
	if [[ -n "$_default_split_lock_mitigate" ]]; then
		_l_msg2 "kernel.split_lock_mitigate = $_default_split_lock_mitigate"
		_l_set_line arch/x86/kernel/cpu/bus_lock.c '^static unsigned int sysctl_sld_mitigate = [0-9]+;$' \
			"static unsigned int sysctl_sld_mitigate = $_default_split_lock_mitigate;"
	fi
	if [[ -n "$_mglru_min_ttl_ms" ]] && (( _mglru_min_ttl_ms > 0 )); then
		# stored in jiffies, rounded up like the sysfs file does
		_l_msg2 "lru_gen/min_ttl_ms = $_mglru_min_ttl_ms"
		_l_set_line mm/vmscan.c '^static unsigned long lru_gen_min_ttl __read_mostly;$' \
			"static unsigned long lru_gen_min_ttl __read_mostly = DIV_ROUND_UP(${_mglru_min_ttl_ms}UL * HZ, MSEC_PER_SEC);"
	fi

	local flag
	case "$_thp_defrag" in
		always)        flag='(1<<TRANSPARENT_HUGEPAGE_DEFRAG_DIRECT_FLAG)|' ;;
		defer)         flag='(1<<TRANSPARENT_HUGEPAGE_DEFRAG_KSWAPD_FLAG)|' ;;
		defer+madvise) flag='(1<<TRANSPARENT_HUGEPAGE_DEFRAG_KSWAPD_OR_MADV_FLAG)|' ;;
		madvise)       flag='(1<<TRANSPARENT_HUGEPAGE_DEFRAG_REQ_MADV_FLAG)|' ;;
		never)         flag='/* lunar: defrag=never */' ;;
	esac
	_l_msg2 "transparent_hugepage/defrag = $_thp_defrag"
	_l_set_line mm/huge_memory.c '^\t\(1<<TRANSPARENT_HUGEPAGE_DEFRAG_REQ_MADV_FLAG\)\|$' "\t$flag"
	if ! _l_true "$_thp_khugepaged_defrag"; then
		_l_msg2 "transparent_hugepage/khugepaged/defrag = 0"
		_l_set_line mm/huge_memory.c '^\t\(1<<TRANSPARENT_HUGEPAGE_DEFRAG_KHUGEPAGED_FLAG\)\|$' \
			'\t/* lunar: khugepaged/defrag=0 */'
	fi
	if [[ -n "$_thp_khugepaged_alloc_sleep_ms" ]]; then
		_l_msg2 "transparent_hugepage/khugepaged/alloc_sleep_millisecs = $_thp_khugepaged_alloc_sleep_ms"
		_l_set_line mm/khugepaged.c '^static unsigned int khugepaged_alloc_sleep_millisecs __read_mostly = [0-9]+;$' \
			"static unsigned int khugepaged_alloc_sleep_millisecs __read_mostly = $_thp_khugepaged_alloc_sleep_ms;"
	fi
	if [[ -n "$_thp_khugepaged_scan_sleep_ms" ]]; then
		_l_msg2 "transparent_hugepage/khugepaged/scan_sleep_millisecs = $_thp_khugepaged_scan_sleep_ms"
		_l_set_line mm/khugepaged.c '^static unsigned int khugepaged_scan_sleep_millisecs __read_mostly = [0-9]+;$' \
			"static unsigned int khugepaged_scan_sleep_millisecs __read_mostly = $_thp_khugepaged_scan_sleep_ms;"
	fi
	if [[ -n "$_thp_khugepaged_pages_to_scan" ]]; then
		_l_msg2 "transparent_hugepage/khugepaged/pages_to_scan = $_thp_khugepaged_pages_to_scan"
		_l_set_line mm/khugepaged.c '^\tkhugepaged_pages_to_scan = HPAGE_PMD_NR \* 8;$' \
			"\tkhugepaged_pages_to_scan = $_thp_khugepaged_pages_to_scan;"
	fi
	if [[ -n "$_thp_khugepaged_max_ptes_swap" ]]; then
		_l_msg2 "transparent_hugepage/khugepaged/max_ptes_swap = $_thp_khugepaged_max_ptes_swap"
		_l_set_line mm/khugepaged.c '^\tkhugepaged_max_ptes_swap = HPAGE_PMD_NR / 8;$' \
			"\tkhugepaged_max_ptes_swap = $_thp_khugepaged_max_ptes_swap;"
	fi
}

# Kconfig helpers
_l_cfg() { scripts/config --file .config "$@"; }

lunar_configure() {
	local where="$1"
	_l_msg "Configuring"

	[[ -s "$where/.lunar-base.config" ]] || _l_die "Base config missing, run ./lunar.sh"
	cp "$where/.lunar-base.config" .config

	_l_cfg --set-str LOCALVERSION "" -d LOCALVERSION_AUTO
	_l_cfg -e IKCONFIG -e IKCONFIG_PROC   # lets _base_config="running" reuse this kernel's config

	# compiler / LTO
	case "$_lto_mode" in
		none) _l_cfg -e LTO_NONE -d LTO_CLANG_THIN -d LTO_CLANG_FULL ;;
		thin) _l_cfg -d LTO_NONE -e LTO_CLANG_THIN -d LTO_CLANG_FULL ;;
		full) _l_cfg -d LTO_NONE -d LTO_CLANG_THIN -e LTO_CLANG_FULL ;;
	esac

	# CPU
	case "$_march" in
		native)  _l_cfg -e X86_NATIVE_CPU ;;
		*)       _l_cfg -d X86_NATIVE_CPU ;;
	esac

	# timer
	_l_cfg -d HZ_100 -d HZ_250 -d HZ_300 -d HZ_1000 -e "HZ_$_timer_freq" --set-val HZ "$_timer_freq"

	# preemption
	case "$_preempt" in
		lazy) _l_cfg -d PREEMPT -e PREEMPT_LAZY -d PREEMPT_RT ;;
		full) _l_cfg -e PREEMPT -d PREEMPT_LAZY -d PREEMPT_RT ;;
		rt)   _l_cfg -e EXPERT -d PREEMPT -d PREEMPT_LAZY -e PREEMPT_RT ;;
	esac
	if _l_true "$_preempt_dynamic"; then _l_cfg -e PREEMPT_DYNAMIC; else _l_cfg -d PREEMPT_DYNAMIC; fi

	# ticks
	case "$_tickless" in
		periodic) _l_cfg -d NO_HZ_IDLE -d NO_HZ_FULL -d NO_HZ -d NO_HZ_COMMON -e HZ_PERIODIC ;;
		idle)     _l_cfg -d HZ_PERIODIC -d NO_HZ_FULL -e NO_HZ_IDLE -e NO_HZ -e NO_HZ_COMMON ;;
		full)     _l_cfg -d HZ_PERIODIC -d NO_HZ_IDLE -d CONTEXT_TRACKING_FORCE -e NO_HZ_FULL -e NO_HZ -e NO_HZ_COMMON -e CONTEXT_TRACKING ;;
	esac

	[[ -n "$_nr_cpus" ]] && _l_cfg --set-val NR_CPUS "$_nr_cpus"

	# default cpufreq governor (one of a Kconfig choice; the governor itself
	# is selected by the choice)
	local g
	for g in PERFORMANCE POWERSAVE USERSPACE ONDEMAND CONSERVATIVE SCHEDUTIL; do
		_l_cfg -d "CPU_FREQ_DEFAULT_GOV_$g"
	done
	_l_cfg -e CPU_FREQ -e "CPU_FREQ_DEFAULT_GOV_${_cpu_governor^^}" -e "CPU_FREQ_GOV_${_cpu_governor^^}"

	# memory: what the lunar patches and their measurement tools rely on
	# (IRQ_TIME_ACCOUNTING: exact interrupt CPU time and /proc/pressure/irq
	# for devwatch.sh and LunarDeviceGuard)
	_l_cfg -e TRANSPARENT_HUGEPAGE -e COMPACTION -e MIGRATION -e PSI -d PSI_DEFAULT_DISABLED \
	       -e SWAP -e MEMCG -e IRQ_TIME_ACCOUNTING
	case "$_thp_enabled" in
		always)  _l_cfg -e TRANSPARENT_HUGEPAGE_ALWAYS -d TRANSPARENT_HUGEPAGE_MADVISE -d TRANSPARENT_HUGEPAGE_NEVER ;;
		madvise) _l_cfg -d TRANSPARENT_HUGEPAGE_ALWAYS -e TRANSPARENT_HUGEPAGE_MADVISE -d TRANSPARENT_HUGEPAGE_NEVER ;;
		never)   _l_cfg -d TRANSPARENT_HUGEPAGE_ALWAYS -d TRANSPARENT_HUGEPAGE_MADVISE -e TRANSPARENT_HUGEPAGE_NEVER ;;
	esac
	if [[ -n "$_thp_shmem_enabled" ]]; then
		local m
		for m in NEVER ALWAYS WITHIN_SIZE ADVISE; do _l_cfg -d "TRANSPARENT_HUGEPAGE_SHMEM_HUGE_$m"; done
		_l_cfg -e "TRANSPARENT_HUGEPAGE_SHMEM_HUGE_${_thp_shmem_enabled^^}"
	fi
	if _l_true "$_mglru"; then _l_cfg -e LRU_GEN -e LRU_GEN_ENABLED; else _l_cfg -d LRU_GEN_ENABLED; fi

	make "${BUILD_FLAGS[@]}" olddefconfig >/dev/null

	if _l_true "$_localmodconfig"; then
		local lsmod_file
		if [[ -s "$_modprobeddb_path" ]]; then
			lsmod_file="$_modprobeddb_path"
			_l_msg2 "localmodconfig with modprobed-db ($lsmod_file)"
		else
			lsmod_file="$(mktemp)"
			lsmod > "$lsmod_file"
			_l_warn "No modprobed-db database at $_modprobeddb_path; using the currently loaded modules only"
		fi
		yes "" | make "${BUILD_FLAGS[@]}" LSMOD="$lsmod_file" localmodconfig >/dev/null
		[[ "$lsmod_file" == "$_modprobeddb_path" ]] || rm -f "$lsmod_file"
	fi

	if [[ "$_menuconfig" != none ]]; then
		make "${BUILD_FLAGS[@]}" "$_menuconfig"
	fi
	make "${BUILD_FLAGS[@]}" olddefconfig >/dev/null

	lunar_check_config
	make -s kernelrelease > version
	_l_msg2 "Kernel release: $(<version)"
	cp .config "$where/lunar-config/last-config-$(<version)"
	_l_msg2 "Final config saved as lunar-config/last-config-$(<version)"
}

# Warn when Kconfig silently dropped a choice (missing dependency etc.).
lunar_check_config() {
	local want=() w
	want+=("CONFIG_HZ=$_timer_freq")
	case "$_lto_mode" in
		thin) want+=(CONFIG_LTO_CLANG_THIN=y) ;;
		full) want+=(CONFIG_LTO_CLANG_FULL=y) ;;
	esac
	case "$_preempt" in
		lazy) want+=(CONFIG_PREEMPT_LAZY=y) ;;
		full) want+=(CONFIG_PREEMPT=y) ;;
		rt)   want+=(CONFIG_PREEMPT_RT=y) ;;
	esac
	[[ "$_march" == native ]] && want+=(CONFIG_X86_NATIVE_CPU=y)
	want+=("CONFIG_CPU_FREQ_DEFAULT_GOV_${_cpu_governor^^}=y")
	want+=("CONFIG_TRANSPARENT_HUGEPAGE_${_thp_enabled^^}=y" CONFIG_COMPACTION=y CONFIG_PSI=y)
	want+=(CONFIG_IRQ_TIME_ACCOUNTING=y)
	[[ -n "$_thp_shmem_enabled" ]] && want+=("CONFIG_TRANSPARENT_HUGEPAGE_SHMEM_HUGE_${_thp_shmem_enabled^^}=y")
	_l_true "$_mglru" && want+=(CONFIG_LRU_GEN_ENABLED=y)
	for w in "${want[@]}"; do
		grep -qx "$w" .config || _l_warn "Wanted $w, but Kconfig did not keep it (missing dependency?)"
	done
}

# Full prepare() sequence.
lunar_prepare() {
	local where="$1"
	_l_msg "Preparing $_srcname for $pkgbase"
	echo "-${pkgrel}" > localversion.10-pkgrel
	echo "-${_pkgsuffix}${_localversion}" > localversion.20-pkgname
	lunar_apply_patches "$where"
	lunar_set_defaults
	lunar_configure "$where"
}
