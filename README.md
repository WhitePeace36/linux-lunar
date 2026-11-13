# linux-lunar

A linux-tkg-style kernel builder for Arch Linux and CachyOS, with one purpose:
**fewer memory-allocation stalls while gaming**. It builds a vanilla kernel.org
kernel plus only the lunar patches below; no scheduler patch sets, no other
third-party changes.

## Quick start

```bash
git clone https://github.com/WhitePeace36/linux-lunar.git && cd linux-lunar
$EDITOR customization.cfg        # or ~/.config/linux-lunar.cfg for your overrides
./lunar.sh                       # asks for the kernel version, builds, installs
```

Then reboot into `linux-lunar` (the bootloader entry is created by
mkinitcpio/dracut and your bootloader's hooks like for any Arch kernel).

| Command | Does |
| --- | --- |
| `./lunar.sh` | resolve options, build, install (`makepkg -si`) |
| `./lunar.sh --build-only` | build the packages only |
| `./lunar.sh --prepare` | download, patch and configure into `src/` to inspect |
| `./lunar.sh --list` | current kernel.org releases and which have lunar patches |
| `./lunar.sh --clean` | remove build output and caches |
| `./lunar.sh -- <makepkg options>` | pass options on to makepkg |

`./lunar.sh` asks which kernel to build and offers the newest stable and the
newest mainline release from kernel.org (`_version="stable"` or `"mainline"`
picks one without asking). Patch sets live in `lunar-patches/<version>/`, named
after a series (`7.3`, used for every 7.3.x) or a single release (`7.3.3`). The
newest set that is not newer than the kernel is used. If the kernel is newer
than every set (a new point release or series before the patches were updated),
that older set is tried unchanged: when every patch applies exactly, the build
continues as usual; when one does not, the build stops with an error and
nothing is built or installed. `_patch_fallback="false"` turns this off.

For PGP-verified stable tarballs, import the kernel.org keys once:
`gpg --locate-keys torvalds@kernel.org gregkh@kernel.org`
(or set `_pgp_verify="false"`).

## The lunar patches

Each can be switched off in `customization.cfg`.

| # | Option | Effect |
| --- | --- | --- |
| 0001 | `_patch_filemap_large_folios` | Page-cache folios larger than 4 KiB are allocated without direct reclaim/compaction; kswapd/kcompactd are woken instead and readahead steps down one order at a time. Plain 4 KiB pages still reclaim when memory is short. Removes reclaim and compaction stalls when games stream assets. (8-32 KiB folios were added after `memstress` showed 1 MiB reads of up to 35 ms spending three quarters of their time in direct reclaim for 32 KiB blocks; that part is not yet tested.) |
| 0002 | `_patch_boost_min_wmark` | `watermark_boost_factor` no longer raises the **min** watermark, so a fragmentation event cannot push allocations into direct reclaim. |
| 0003 | `_patch_ttm_low_latency` | `ttm.low_latency_alloc`: how hard GPU buffer allocations (amdgpu, xe and other TTM drivers) work for 2 MiB pages. 0 = retry hard (upstream), 1 = one bounded reclaim/compaction round, 2 = no reclaim/compaction in the calling thread, only kswapd is woken. Every level falls back to smaller pages. |
| 0004 | `_patch_soft_defrag_mode` | `vm.defrag_soft`: with `vm.defrag_mode=1`, kswapd/kcompactd still keep whole free 2 MiB blocks, but allocations never stall just to avoid a migratetype fallback. Opportunistic attempts that can't reclaim at all (SLUB's larger slabs, ...) don't fall back either; they retry smaller, so they can't fragment the free blocks. A separate switch instead of `defrag_mode=2`, because upstream keeps that value for a stricter mode (prefer OOM over fragmentation). |
| 0005 | `_patch_kswapd_hopeless` | A kswapd that gave up ("hopeless") is revived by its own or by global direct-reclaim progress again, not only once free memory is back above the high watermark. |
| 0006 | `_patch_lru_drain_bounded` | `vm.lru_drain_timeout_ms`: khugepaged, MADV_COLLAPSE, `fadvise(DONTNEED)`, KSM, `memory.reclaim` and `drop_caches` stop waiting after 20 ms for a CPU that can't run its LRU drain work (interrupt storm, stuck driver, ...), instead of stalling, and everyone queued behind them with it. Callers that need the drain (migration, GUP pins, ...) still wait, but the kernel log now names the stuck CPU after 1 s. Counters: `lru_drain_timeout`, `lru_drain_slow` in `/proc/vmstat`. |
| 0007 | `_patch_khugepaged_prio` | khugepaged ran at nice 19, also while holding a program's `mmap_lock`, VMA and anon_vma locks or page cache folio locks during a collapse (and, with `khugepaged/defrag=1`, the pages its compaction moves). On a busy CPU it could then wait a long time to run again, and the program's `mmap`/`munmap`/`mprotect` calls and some page faults waited with it. It now runs at nice 0. |
| 0008 | `_patch_direct_reclaim_split` | `vm.thp_direct_reclaim_split`: swapping a 2 MiB huge page out to zram compresses all of it in the task that triggered reclaim, 3-4 ms with zstd, and with THP `always` almost all program memory is huge pages. When memory fills up quickly, every task that allocates then (PipeWire, the compositor, the browser, the game) can be the one paying for it. In direct reclaim the huge page is now split, only about the 32 pages the task needs are swapped out, and kswapd handles the rest later. kswapd, `memory.reclaim` and MADV_PAGEOUT still swap out huge pages whole. Counter: `thp_swpout_direct_split` in `/proc/vmstat`. |

## Compiled-in defaults

Set in `customization.cfg`, all still changeable at runtime:

| Setting | lunar default | upstream |
| --- | --- | --- |
| `vm.defrag_mode` / `vm.defrag_soft` | 1 / 1 | 0 / (no such setting) |
| `vm.watermark_scale_factor` | 500 | 10 |
| `vm.watermark_boost_factor` | 600 | 15000 |
| `vm.compaction_proactiveness` | 50 | 20 |
| `vm.swappiness` | 150 | 60 |
| `ttm.low_latency_alloc` | 1 | 0 |
| `vm.lru_drain_timeout_ms` | 20 | (patch 0006 only) |
| `vm.thp_direct_reclaim_split` | 1 | (patch 0008 only) |
| THP enabled / defrag | always / defer | Kconfig / madvise |
| khugepaged defrag / alloc_sleep_millisecs | 0 / 2500 | 1 / 60000 |
| khugepaged pages_to_scan / scan_sleep_millisecs | 8192 / 2500 | 4096 / 10000 |
| khugepaged max_ptes_swap | 0 | 64 |
| THP shmem_enabled | always | Kconfig / never |
| `vm.dirty_bytes` / `vm.dirty_background_bytes` | 512 MiB / 64 MiB | 0 (20 % / 10 % ratio) |
| `vm.dirty_writeback_centisecs` / `vm.dirty_expire_centisecs` | 100 / 500 | 500 / 3000 |
| `vm.max_map_count` | 2147483642 | 65530 |
| `split_lock_detect` / `kernel.split_lock_mitigate` | off / 0 | warn / 1 |

`watermark_boost_factor` is a percentage of the high watermark, so it is kept
small when `watermark_scale_factor` is large (600 ≈ 175 MB on 32 GiB with 500).

`compaction_proactiveness` 50: in `memstress -a 16` runs, 80 halved THP
fallbacks compared with 20 but caused 10-37 ms hitches in streaming reads in
every run (proactive compaction migrates page-cache pages that are being read);
50 kept most of the THP benefit without them.

`khugepaged/defrag` 0: khugepaged merges small pages into a huge page only when
a free 2 MiB block is there. It no longer reclaims memory (with swappiness 150
possibly pushing a game's memory to zram) or compacts it just for a merge;
kswapd and kcompactd produce the free blocks (`defrag_mode=1`, THP `defer`).
After a failed allocation it tries again after 2.5 s instead of 60 s; with
defrag 0 a failed attempt only checks the free lists, so retrying soon costs
nothing. If the retry fails too, the pass ends and the normal 2.5 s pause
between passes follows.

khugepaged scans 8× as fast as upstream (8192 pages every 2.5 s instead of
4096 every 10 s), so new memory becomes huge pages sooner. `max_ptes_swap` 0:
it never reads memory back from zram/swap just to merge a range; such ranges
are merged once the program has touched them again itself.

`dirty_expire_centisecs` 500: written data reaches the disk within about
5-6 s instead of up to 30 s, so a crash or power loss loses less. It does not
change stalls: large writes start background writeback at 64 MiB anyway.

`lru_gen/min_ttl_ms` stays at the upstream 0 (off). With 1000, kswapd's OOM
kill fired during a `memstress -a 8` burst with 13 GiB of zram still free; the
same run completed without it.

`split_lock_detect=off`: with the upstream "warn", every split lock (some
games do them constantly) makes the kernel slow the program down on purpose.
CPUs without split lock or bus lock detection are not affected either way.
The `split_lock_detect=` boot parameter still overrides the default.

Build defaults: GCC, `-march=native`, 1000 Hz, `PREEMPT` (full) with
PREEMPT_DYNAMIC, periodic tick, `performance` CPU governor, THP `always`,
MGLRU on, cgroup `favordynmods` on (`CGROUP_FAVOR_DYNMODS`: starting a service
or app no longer blocks fork/exec/exit system-wide while its process is moved
into a cgroup), zswap off by default (`ZSWAP_DEFAULT_ON`; in front of zram it
would compress everything twice).

## Other options

Base config (Arch, CachyOS, running kernel, file), `localmodconfig` with
modprobed-db, menuconfig/nconfig/xconfig, GCC or LLVM with Thin/Full LTO,
`-march` (native, generic, x86-64-v3, znver4, ...), extra KCFLAGS, timer
frequency, preemption (lazy/full/rt) and PREEMPT_DYNAMIC, tick mode, NR_CPUS,
default CPU governor (performance/schedutil/powersave), THP, MGLRU, headers
package. See the comments in `customization.cfg`.

## Interrupting and cleaning up

Ctrl+C, closing the terminal, or `kill` stops every build
process (makepkg, make, compilers) and removes `src/`, `pkg/` and partial
downloads (`_cleanup_on_abort`; set it to `"false"` to keep `src/` for
debugging). A failed build is cleaned up the same way. After a successful build
`src/` and `pkg/` are removed too (`_cleanup_after_build`); the packages are
kept. The kernel tarball and its signature are downloaded to `sources/`; after a
build only those of the version just built stay there, so a rebuild skips the
download (`_keep_kernel_source="false"` removes `sources/` as well). A running
`pacman` installation is never interrupted. `./lunar.sh --clean` removes
everything: `src/`, `pkg/`, `sources/`, packages and cached downloads.

## Your own patches

Like linux-tkg: put `*.mypatch` (applied) and `*.myrevert` (reverse-applied
first) into `linux<XY>-lunar-userpatches/` next to `lunar.sh`, e.g.
`linux73-lunar-userpatches/`. Unlike linux-tkg, a patch that fails stops the
build.

## Measuring

`tools/` holds what was used to test the patches. Build `memstress` first:
`gcc -O2 -pthread -o tools/memstress tools/memstress.c`

| Tool | Use |
| --- | --- |
| `memcheck.sh` | show every relevant setting and the live per-zone watermarks / kswapd state |
| `memdiff.sh` | counter deltas (allocstall, compact_stall, THP, PSI, ...) across a gaming session |
| `memstress.c` | provoke stalls: fragmented page cache, pinned unmovable pages, THP heap, streaming reads, optional 2:1-compressible anonymous load (`-a GiB`) for zram |
| `memstall.bt` | bpftrace: every direct reclaim / compaction stall > 1 ms with process and order |
| `fragwatch.sh` | live fragmentation score, free 2 MiB blocks, kswapd state, compaction migrations and THP success per second |
| `devwatch.sh` | find a misbehaving device: PSI irq/cpu/io/mem, busiest IRQ lines, busiest disks (busy %, wait, stuck requests), tasks blocked in D state with their kernel stack, new PCIe AER errors per device, busiest kernel threads, new kernel errors and USB disconnects (`sudo ./devwatch.sh 1`) |
| `ipiwho.sh` | which kernel functions CPUs send each other (the `CAL` interrupts), from where and for which task, recorded for a few seconds via the `csd:csd_queue_cpu` tracepoint (`sudo ./ipiwho.sh 5`) |
| `stallwho.bt` | bpftrace: every time a thread of one process is blocked in the kernel (D state) for longer than a threshold, with where it was blocked and **who woke it up**, with the waker's kernel stack. Shows which lock or background thread a game waited for (`sudo bpftrace tools/stallwho.bt <pid> [ms]`) |
| `draintest.c` | reproduce the stall patch 0006 fixes: an RT task on one CPU keeps that CPU's LRU cache full while `drop_caches` / `compact_memory` wait for it (`gcc -O2 -o tools/draintest tools/draintest.c`) |

`memstress -q <sec> -B <MiB> -t 0` tests proactive compaction on its own: after
the fragmenting setup it stays idle for `<sec>` seconds, then touches one THP
burst and reports how many 2 MiB chunks got a huge page.

## LunarDeviceGuard

Containing misbehaving devices (IRQ storms, PCIe error floods, USB reconnect
loops) and catching short freezes is a separate, kernel-independent project:
[LunarDeviceGuard](https://github.com/WhitePeace36/LunarDeviceGuard).

## Origin

The patches and scripts were written with an AI assistant (Claude) during an
analysis of the kernel's allocator, kswapd, kcompactd and khugepaged. They are
drafts. Patch 0005 matches an approach already under discussion upstream
("mm/vmscan: clear hopeless kswapd when global direct reclaim makes progress",
linux-mm, July 2026), where it is disputed. Upstream submissions must follow
`Documentation/process/coding-assistants.rst` and carry a human Signed-off-by.

Everything in this repository is licensed under GPL-2.0 (see `LICENSE`), like
the kernel the patches are for. Packaging follows the structure of Arch Linux's
`linux` PKGBUILD.
