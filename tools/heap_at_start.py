#!/usr/bin/env python3
"""The heap at the moment ESP-IDF creates the main task - does this image start? (task p11-01)

v0.1.3 was a signed, verified, correctly flashed image that could not run a line of this
firmware: ESP-IDF could not create the main task, and every boot ended in

    assert failed: esp_startup_start_app app_startup.c:86 (res == pdTRUE)

The linker said 37 037 bytes of DIRAM were free. That figure does not decide anything: it is the
room left in the segment, and what decides whether the image starts is the largest *free block* of
one heap region after ESP-IDF's start-up and this firmware's static initialisers have taken what
they take - 86 KB of it, before `app_main`. This script is that calculation, and it answers the
only question that matters: when `xTaskCreatePinnedToCore` asks for CONFIG_ESP_MAIN_TASK_STACK_SIZE
bytes, is there a block that holds it, and how much is left over.

    python tools/heap_at_start.py --build-dir /build/fw

Run it in the pinned ESP-IDF image (tools/verify.ps1 -Shell): it needs the build's `sdkconfig`,
`twinoak-walter.map` and `twinoak-walter.elf`, and pyelftools, which ESP-IDF ships.

--------------------------------------------------------------------------------------------------
Where every number in here comes from
--------------------------------------------------------------------------------------------------
Nothing is fitted to an outcome and nothing is rounded to taste. Three sources, all citable:

1. **The build.** `_heap_start` and the other segment bounds from the linker map; the options that
   shape the heap from `sdkconfig`; `sizeof(mm::JobRunner)`, `sizeof(TCB_t)` and
   `agent::kKeptScratchBytes` from the ELF's DWARF.
2. **ESP-IDF v5.5.5, as it is in the pinned image.** The region table and the per-type capabilities
   are `components/heap/port/esp32s3/memory_layout.c`; the trimming of reserved regions and the
   coalescing are `components/heap/port/memory_layout_utils.c` and
   `components/heap/heap_caps_init.c` (which also sorts the registered heaps by *increasing size* -
   the reason a small allocation is not always where you would guess); the order the heaps are
   tried in is `components/heap/heap_caps_base.c`; the allocator is `components/heap/tlsf/` -
   `tlsf_control_functions.h` for `mapping_search`, which rounds an allocation up to the next
   second-level size class and is why a 66 000-byte object takes a 69 632-byte block; the main
   task's stack and TCB are `components/freertos/app_startup.c` and
   `components/freertos/heap_idf.c` (`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`). The ROM's own
   reserved DRAM comes from the ROM layout table, which is data in ROM and not in any header:
   `ets_rom_layout` at 0x3FF1AE90 in `esp32s3_rev0_rom.elf` (the `esp-rom-elfs` tool of the
   image), whose second word - `dram0_rtos_reserved_start` - is 0x3FCEEE34:

       xtensa-esp32s3-elf-objdump -s --start-address=0x3ff1ae90 --stop-address=0x3ff1aeb0 \\
           /opt/esp/tools/esp-rom-elfs/*/esp32s3_rev0_rom.elf

3. **The start-up's own allocations**, in the order they happen, from the file given by
   `--programme` (default `tools/heap_at_start.programme`). That list is not guessed either: it was
   recorded from this firmware booting in the QEMU of the pinned image, with a breakpoint on
   `heap_caps_aligned_alloc_base` and one on `heap_caps_free` - `tools/heap_programme.gdb` is the
   recorder and the programme file says how to run it. The two allocations that change with the
   build are named rather than numbered (`runner`, `scratch`) and their sizes come from the DWARF.

Which means the result is checkable against the chip, and was: for v0.1.2 and v0.1.3 this script's
block map is byte for byte the one read out of the running image in QEMU, and its verdict is what
`heap_caps_malloc(16384, MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)` really answered there - a pointer
for v0.1.2, NULL for v0.1.3. See tasks/phase-11/reports/01.md.

Exit code: 0 when the main task fits with at least --min-spare bytes left in the block it gets,
1 when it does not. Since task p11-02 that is **rail R22**: tools/verify.sh's build step runs this
over every build it makes, at --min-spare $HEAP_MIN_SPARE (8 192), before a byte of it is copied
to out/images/ or released, so an image that cannot start is never published.

The last section is for the board rather than for the build. *What the main task leaves behind* is
the arrangement this firmware's own `heap:` boot line reports - a different one, because by then
the stack is taken and the region the ROM's stack was in has been registered as a heap - and it is
an **upper bound** on it: the recording stops where the main task is created, and FreeRTOS's idle
and timer tasks, the task watchdog and app_main's first lines take about 7 kB more after it.
docs/bench.md §1 holds a board to figures measured in QEMU, not to this one.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# ---- ESP-IDF v5.5.5, esp32s3 -------------------------------------------------------------------
# soc/soc.h
SOC_DIRAM_DRAM_LOW = 0x3FC88000
SOC_DIRAM_DRAM_HIGH = 0x3FCF0000
SOC_DIRAM_IRAM_LOW = 0x40378000
SOC_RTC_DRAM_LOW = 0x600FE000
SOC_RTC_DRAM_HIGH = 0x60100000
SOC_ROM_STACK_START = 0x3FCEB710
SOC_ROM_STACK_SIZE = 0x2000
I_D_OFFSET = SOC_DIRAM_IRAM_LOW - SOC_DIRAM_DRAM_LOW
# heap/port/esp32s3/memory_layout.c: APP_USABLE_DRAM_END
APP_USABLE_DRAM_END = SOC_ROM_STACK_START - SOC_ROM_STACK_SIZE
# The ROM layout table's dram0_rtos_reserved_start (see the module docstring for how to read it).
ROM_DRAM0_RTOS_RESERVED_START = 0x3FCEEE34

# heap/include/esp_heap_caps.h
CAP_EXEC = 1 << 0
CAP_32BIT = 1 << 1
CAP_8BIT = 1 << 2
CAP_DMA = 1 << 3
CAP_SPIRAM = 1 << 10
CAP_INTERNAL = 1 << 11
CAP_DEFAULT = 1 << 12
CAP_RETENTION = 1 << 14
CAP_RTCRAM = 1 << 15
CAP_SIMD = 1 << 20
COMMON_CAPS = CAP_DEFAULT | CAP_32BIT | CAP_8BIT  # ESP32S3_MEM_COMMON_CAPS

CAP_NAMES = [
    (CAP_EXEC, "EXEC"), (CAP_32BIT, "32BIT"), (CAP_8BIT, "8BIT"), (CAP_DMA, "DMA"),
    (CAP_SPIRAM, "SPIRAM"), (CAP_INTERNAL, "INTERNAL"), (CAP_DEFAULT, "DEFAULT"),
    (CAP_RETENTION, "RETENTION"), (CAP_RTCRAM, "RTCRAM"), (CAP_SIMD, "SIMD"),
]

# tlsf/tlsf_block_functions.h and tlsf_control_functions.h
ALIGN_SIZE_LOG2 = 2
ALIGN_SIZE = 1 << ALIGN_SIZE_LOG2
BLOCK_HEADER_OVERHEAD = 4          # sizeof(size_t)
BLOCK_HEADER_SIZE = 16             # sizeof(block_header_t)
BLOCK_SIZE_MIN = BLOCK_HEADER_SIZE - 4
CONTROL_T_SIZE = 36                # block_null(16) + the bitfield word + size + fl_bitmap + 2 ptrs
# multi_heap.c: the per-heap bookkeeping that lives at the start of the region
MULTI_HEAP_INFO_SIZE = 20          # {lock, free_bytes, minimum_free_bytes, pool_size, heap_data}
# heap/heap_private.h: sizeof(heap_t) - caps[3], start, end, a portMUX, the handle and the link.
# heap_caps_init() allocates one array of these for all the heaps it just registered, and that is
# the first thing ever taken from the heap. It goes through multi_heap_malloc and not through
# heap_caps_malloc, so a recording made at heap_caps_aligned_alloc_base cannot see it; the model
# does it itself, before the programme's first line.
HEAP_T_SIZE = 36

# freertos/heap_idf.c: portFREERTOS_HEAP_CAPS - every task stack and TCB
FREERTOS_CAPS = CAP_INTERNAL | CAP_8BIT


def caps_str(caps: int) -> str:
    names = [n for bit, n in CAP_NAMES if caps & bit]
    return "|".join(names) if names else "-"


def align_up(x: int, a: int) -> int:
    return (x + a - 1) & ~(a - 1)


def align_down(x: int, a: int) -> int:
    return x & ~(a - 1)


def fls(x: int) -> int:
    """tlsf_fls: the index of the highest set bit, -1 for 0."""
    return x.bit_length() - 1


# ---- the allocator ------------------------------------------------------------------------------
class Block:
    __slots__ = ("ptr", "size", "free", "op", "took")

    def __init__(self, ptr: int, size: int, free: bool, op: str = ""):
        self.ptr = ptr
        self.size = size
        self.free = free
        self.op = op
        # For an allocated block: the size of the free block it was carved out of. The difference
        # is how much room this allocation had to spare in the heap it landed in - and the
        # smallest of those differences is how far this build is from landing something somewhere
        # else, which is how v0.1.3 came about.
        self.took = 0


class Tlsf:
    """One multi_heap over one region, faithful to components/heap/tlsf at v5.5.5.

    Only what a start-up does is modelled: malloc of a word-aligned size, free, and the physical
    coalescing and segregated free lists that decide where the next one lands. There is no
    realloc, no aligned_alloc and no poisoning (CONFIG_HEAP_POISONING_DISABLED).
    """

    def __init__(self, start: int, size: int):
        self.start = start
        self.region_size = size
        pool_bytes = size - MULTI_HEAP_INFO_SIZE
        # control_construct(): the second level is chosen from the pool's size
        self.fl_index_max = pool_bytes.bit_length()
        if pool_bytes <= 16 * 1024:
            self.sl_log2 = 3
        elif pool_bytes <= 256 * 1024:
            self.sl_log2 = 4
        else:
            self.sl_log2 = 5
        self.fl_shift = self.sl_log2 + ALIGN_SIZE_LOG2
        self.sl_count = 1 << self.sl_log2
        self.fl_count = self.fl_index_max - self.fl_shift + 1
        self.small_block_size = 1 << self.fl_shift
        self.control_size = (CONTROL_T_SIZE + 4 * self.fl_count
                             + 4 * self.fl_count * self.sl_count)
        # tlsf_create_with_pool() then tlsf_add_pool(): one free block, two headers of overhead
        mem = start + MULTI_HEAP_INFO_SIZE + self.control_size
        first = align_down(pool_bytes - self.control_size - 2 * BLOCK_HEADER_OVERHEAD, ALIGN_SIZE)
        self.blocks = [Block(mem + BLOCK_HEADER_OVERHEAD, first, True)]
        # the pool's sentinel: the zero-size last block nothing merges into
        self.sentinel = Block(mem + BLOCK_HEADER_OVERHEAD + first + BLOCK_HEADER_OVERHEAD, 0, False)
        self.free_bytes = pool_bytes - self.control_size
        self.free_lists: dict[tuple[int, int], list[Block]] = {}
        self._insert(self.blocks[0])

    # -- the mapping functions (tlsf_control_functions.h) --
    def _mapping_insert(self, size: int) -> tuple[int, int]:
        if size < self.small_block_size:
            return 0, size // (self.small_block_size // self.sl_count)
        fl = fls(size)
        sl = (size >> (fl - self.sl_log2)) ^ (1 << self.sl_log2)
        return fl - (self.fl_shift - 1), sl

    def _mapping_search(self, size: int) -> tuple[int, int, int]:
        """Returns (rounded size, fl, sl). The rounding is why a block is bigger than the ask."""
        if size >= self.small_block_size:
            size = align_up(size, 1 << (fls(size) - self.sl_log2))
        fl, sl = self._mapping_insert(size)
        return size, fl, sl

    def _insert(self, block: Block) -> None:
        key = self._mapping_insert(block.size)
        self.free_lists.setdefault(key, []).insert(0, block)  # insert_free_block: at the head

    def _remove(self, block: Block) -> None:
        self.free_lists[self._mapping_insert(block.size)].remove(block)

    def _search_suitable(self, fl: int, sl: int) -> Block | None:
        for s in range(sl, self.sl_count):
            lst = self.free_lists.get((fl, s))
            if lst:
                return lst[0]
        for f in range(fl + 1, self.fl_count):
            for s in range(self.sl_count):
                lst = self.free_lists.get((f, s))
                if lst:
                    return lst[0]
        return None

    def malloc(self, size: int, op: str = "") -> Block | None:
        adjust = max(align_up(size, ALIGN_SIZE), BLOCK_SIZE_MIN)
        if adjust >= (1 << self.fl_index_max):
            return None
        adjust, fl, sl = self._mapping_search(adjust)
        if fl >= self.fl_count:
            return None
        block = self._search_suitable(fl, sl)
        if block is None:
            return None
        self._remove(block)
        took = block.size
        # block_prepare_used -> block_trim_free: split only when the remainder can hold a header
        if block.size >= BLOCK_HEADER_SIZE + adjust:
            rest = Block(block.ptr + adjust + BLOCK_HEADER_OVERHEAD,
                         block.size - adjust - BLOCK_HEADER_OVERHEAD, True)
            block.size = adjust
            self.blocks.insert(self.blocks.index(block) + 1, rest)
            self._insert(rest)
        block.free = False
        block.op = op
        block.took = took
        self.free_bytes -= block.size + BLOCK_HEADER_OVERHEAD
        return block

    def free(self, block: Block) -> None:
        assert not block.free
        self.free_bytes += block.size + BLOCK_HEADER_OVERHEAD
        block.free = True
        block.op = ""
        i = self.blocks.index(block)
        if i + 1 < len(self.blocks) and self.blocks[i + 1].free:          # block_merge_next
            nxt = self.blocks[i + 1]
            self._remove(nxt)
            block.size += nxt.size + BLOCK_HEADER_OVERHEAD
            self.blocks.pop(i + 1)
        if i > 0 and self.blocks[i - 1].free:                             # block_merge_prev
            prev = self.blocks[i - 1]
            self._remove(prev)
            prev.size += block.size + BLOCK_HEADER_OVERHEAD
            self.blocks.pop(i)
            block = prev
        self._insert(block)

    def largest_free(self) -> int:
        return max([b.size for b in self.blocks if b.free] or [0])

    def fit_size(self, size: int) -> int:
        """tlsf_fit_size: the largest *request* a free block of `size` bytes is certain to serve.

        It is not the block's size. mapping_search rounds a request up to the next second-level
        class before it looks for a block, so a request between two classes can fail against a
        block that would have held it; tlsf_fit_size rounds down to the class below, and that is
        what heap_caps_get_largest_free_block() returns - "the largest value of s for which
        heap_caps_malloc(s, caps) will succeed" (esp_heap_caps.h; multi_heap_get_info ->
        tlsf_fit_size in tlsf.c). A 28 532-byte block answers 27 648. The two figures have to be
        told apart when this model is held against a board, which is what the last section does.
        """
        if size > self.small_block_size:
            interval = 1 << (fls(size) - self.sl_log2)
            return size & ~(interval - 1)
        return size

    def largest_free_request(self) -> int:
        return self.fit_size(self.largest_free())

    def would_fit(self, size: int) -> bool:
        adjust = max(align_up(size, ALIGN_SIZE), BLOCK_SIZE_MIN)
        adjust, fl, sl = self._mapping_search(adjust)
        return fl < self.fl_count and self._search_suitable(fl, sl) is not None


class Heap:
    def __init__(self, start: int, end: int, caps: list[int], name: str, startup_stack: bool):
        self.start = start
        self.end = end
        self.caps = caps
        self.name = name
        self.startup_stack = startup_stack
        # heap_caps_init(): a startup-stack region is registered only in main_task, which is
        # *after* the main task has been created - so it cannot serve the main task's own stack.
        self.tlsf = None if startup_stack else Tlsf(start, end - start)

    @property
    def size(self) -> int:
        return self.end - self.start

    def all_caps(self) -> int:
        return self.caps[0] | self.caps[1] | self.caps[2]


class HeapCaps:
    """registered_heaps and heap_caps_aligned_alloc_base's three passes."""

    def __init__(self, heaps: list[Heap]):
        self.in_region_order = list(heaps)
        self.heaps: list[Heap] = []
        for h in heaps:  # sorted_add_to_registered_heaps: by increasing size, in region order
            at = len(self.heaps)
            for i, cur in enumerate(self.heaps):
                if cur.size >= h.size:
                    at = i
                    break
            self.heaps.insert(at, h)

    def registered_heaps_array(self) -> tuple[Heap, Block]:
        """heap_caps_init()'s own allocation: the array of heap_t it keeps the heaps in.

        It takes the **first** heap of the region order that can hold it and that is registered -
        not the first of the size-sorted list - because at that point the sorted list does not
        exist yet (heap_caps_init.c).
        """
        size = HEAP_T_SIZE * len(self.in_region_order)
        for h in self.in_region_order:
            if h.tlsf is None:
                continue
            if (h.all_caps() & (CAP_8BIT | CAP_INTERNAL)) != (CAP_8BIT | CAP_INTERNAL):
                continue
            block = h.tlsf.malloc(size, "heap_caps_init: the registered-heaps array")
            if block is not None:
                return h, block
        raise SystemExit("no heap can hold heap_caps_init's own array of %d bytes" % size)

    def malloc(self, size: int, caps: int, op: str = "") -> tuple[Heap, Block] | None:
        for prio in range(3):
            for h in self.heaps:
                if h.tlsf is None:
                    continue
                if (h.caps[prio] & caps) == 0:
                    continue
                if (h.all_caps() & caps) != caps:
                    continue
                block = h.tlsf.malloc(size, op)
                if block is not None:
                    return h, block
        return None

    def would_fit(self, size: int, caps: int) -> Heap | None:
        for prio in range(3):
            for h in self.heaps:
                if h.tlsf is None or (h.caps[prio] & caps) == 0:
                    continue
                if (h.all_caps() & caps) != caps:
                    continue
                if h.tlsf.would_fit(size):
                    return h
        return None


# ---- the build ----------------------------------------------------------------------------------
def read_sdkconfig(path: str) -> dict:
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" in line:
                k, v = line.split("=", 1)
                out[k] = v
    return out


def on(cfg: dict, key: str) -> bool:
    return cfg.get(key) == "y"


MAP_SYMBOL = re.compile(r"^\s+0x([0-9a-f]{8,16})\s+(\w+) = ")


def read_map_symbols(path: str) -> dict:
    """The linker map's absolute symbol lines: `                0x3fcd2770   _heap_start = ...`."""
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = MAP_SYMBOL.match(line)
            if m:
                out.setdefault(m.group(2), int(m.group(1), 16))
    return out


def read_version(build: str) -> str:
    """The version the build was stamped with, from project_description.json if it is there."""
    import json
    path = os.path.join(build, "project_description.json")
    try:
        with open(path, encoding="utf-8") as f:
            return str(json.load(f).get("project_version", "?"))
    except (OSError, ValueError):
        return "?"


def read_dwarf_sizes(path: str) -> dict:
    """sizeof(mm::JobRunner), sizeof(TCB_t) and agent::kKeptScratchBytes, from the ELF."""
    from elftools.elf.elffile import ELFFile

    want_types = {"JobRunner", "tskTaskControlBlock"}
    want_consts = {"kKeptScratchBytes"}
    found = {}
    with open(path, "rb") as f:
        dwarf = ELFFile(f).get_dwarf_info()
        for cu in dwarf.iter_CUs():
            for die in cu.iter_DIEs():
                name = die.attributes.get("DW_AT_name")
                if name is None:
                    continue
                n = name.value.decode(errors="replace")
                if n in want_types and die.tag in ("DW_TAG_structure_type", "DW_TAG_class_type"):
                    size = die.attributes.get("DW_AT_byte_size")
                    if size is not None:
                        found.setdefault(n, size.value)
                elif n in want_consts and die.tag == "DW_TAG_variable":
                    const = die.attributes.get("DW_AT_const_value")
                    if const is not None:
                        found.setdefault(n, const.value)
    return found


# ---- the regions --------------------------------------------------------------------------------
DIRAM, DRAM, IRAM, RTCRAM = "DIRAM", "DRAM", "IRAM", "RTCRAM"


def type_caps(kind: str, memprot: bool) -> list[int]:
    """soc_memory_types[] of heap/port/esp32s3/memory_layout.c, with the MEMPROT branch."""
    diram = COMMON_CAPS | CAP_INTERNAL | CAP_DMA | CAP_RETENTION
    rtc = COMMON_CAPS | CAP_INTERNAL
    if not memprot:
        diram |= CAP_EXEC
        rtc |= CAP_EXEC
    if kind == DIRAM:
        return [diram | CAP_SIMD, 0, 0]
    if kind == DRAM:
        return [0, COMMON_CAPS | CAP_INTERNAL | CAP_DMA | CAP_SIMD, 0]
    if kind == IRAM:
        return [CAP_EXEC, CAP_32BIT | CAP_INTERNAL, 0]
    if kind == RTCRAM:
        return [CAP_RTCRAM, 0, rtc]
    raise AssertionError(kind)


def build_heaps(cfg: dict, sym: dict) -> tuple[list[Heap], list[tuple]]:
    memprot = on(cfg, "CONFIG_ESP_SYSTEM_MEMPROT_FEATURE")
    regions = []  # (start, size, kind, startup_stack)
    if on(cfg, "CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB") and not memprot:
        regions.append((0x40374000, 0x4000, IRAM, False))
    for start in (0x3FC88000,):
        regions.append((start, 0x8000, DIRAM, False))
    for start in (0x3FC90000, 0x3FCA0000, 0x3FCB0000, 0x3FCC0000, 0x3FCD0000):
        regions.append((start, 0x10000, DIRAM, False))
    regions.append((0x3FCE0000, APP_USABLE_DRAM_END - 0x3FCE0000, DIRAM, False))
    regions.append((APP_USABLE_DRAM_END, SOC_DIRAM_DRAM_HIGH - APP_USABLE_DRAM_END, DIRAM, True))
    if on(cfg, "CONFIG_ESP32S3_DATA_CACHE_16KB") or on(cfg, "CONFIG_ESP32S3_DATA_CACHE_32KB"):
        regions.append((0x3FCF0000, 0x8000, DRAM, False))
    if on(cfg, "CONFIG_ESP32S3_DATA_CACHE_16KB"):
        regions.append((0x3C000000, 0x4000, DRAM, False))
    if on(cfg, "CONFIG_SPIRAM"):
        raise SystemExit("this build has PSRAM; the model was written for the board, which has none")
    if on(cfg, "CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP"):
        regions.append((SOC_RTC_DRAM_LOW, 0x2000, RTCRAM, False))

    rtc_data_end = sym["_rtc_noinit_end"] if on(cfg, "CONFIG_ESP32S3_RTCDATA_IN_FAST_MEM") \
        else sym["_rtc_force_fast_end"]
    reserved = [
        (sym["_iram_start"] - I_D_OFFSET, sym["_iram_end"] - I_D_OFFSET, "iram_code"),
        (sym["_data_start"], sym["_heap_start"], "dram_data"),
        (SOC_RTC_DRAM_LOW, rtc_data_end, "rtcram_data"),
        (sym["_rtc_reserved_start"], sym["_rtc_reserved_end"], "rtc_reserved_data"),
        (ROM_DRAM0_RTOS_RESERVED_START, SOC_DIRAM_DRAM_HIGH, "rom (ets_rom_layout)"),
    ]
    if on(cfg, "CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB"):
        reserved.append((sym["_iram_start"], sym["_iram_end"], "iram_code_2"))
    trace = int(cfg.get("CONFIG_ESP32S3_TRACEMEM_RESERVE_DRAM", "0x0"), 0)
    if trace:
        raise SystemExit("this build reserves trace memory; the model does not know that region")
    reserved = sorted(((s & ~3, align_up(e, 4), n) for s, e, n in reserved))

    # soc_get_available_memory_regions(): trim every reserved range out of every region
    avail = []
    for start, size, kind, stack in regions:
        pieces = [(start, start + size)]
        for r0, r1, _ in reserved:
            out = []
            for a, b in pieces:
                if r1 <= a or r0 >= b:
                    out.append((a, b))
                    continue
                if r0 <= a and r1 >= b:
                    continue
                if a < r0 and b > r1:
                    out.append((a, r0))
                    out.append((r1, b))
                elif r0 <= a:
                    out.append((r1, b))
                else:
                    out.append((a, r0))
            pieces = out
        for a, b in pieces:
            if b - a > 16:  # "ignore regions smaller than 16B"
                avail.append((a, b, kind, stack))
    avail.sort()

    # heap_caps_init(): coalesce adjacent regions of the same type and the same startup_stack flag
    merged: list[list] = []
    for a, b, kind, stack in avail:
        if merged and merged[-1][1] == a and merged[-1][2] == kind and merged[-1][3] == stack:
            merged[-1][1] = b
        else:
            merged.append([a, b, kind, stack])

    heaps = [Heap(a, b, type_caps(kind, memprot), kind, stack) for a, b, kind, stack in merged]
    return heaps, reserved


# ---- the programme ------------------------------------------------------------------------------
def read_programme(path: str) -> list[tuple]:
    ops = []
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if parts[0] == "alloc" and len(parts) == 3:
                size = parts[1] if not parts[1].isdigit() else int(parts[1])
                ops.append(("alloc", size, int(parts[2], 0), raw.strip()))
            elif parts[0] == "free" and len(parts) == 2:
                ops.append(("free", int(parts[1]), None, raw.strip()))
            else:
                raise SystemExit("%s:%d: cannot read '%s'" % (path, n, line))
    return ops


RECORD_ALLOC = re.compile(r"^A size=(\d+) caps=0x([0-9a-fA-F]+)")
RECORD_FREE = re.compile(r"^F ptr=0x([0-9a-fA-F]+)")


def write_programme(record: str, cfg: dict, sym: dict, sizes: dict, version: str) -> int:
    """Turn a recording from tools/heap_programme.gdb into a programme this script can replay.

    A recorded free names an address, which is this build's; a programme has to outlive the build,
    so the free is written as the number of the operation that allocated that block - which means
    replaying the recording through the model, and means the model's addresses have to be the
    chip's. They are, or this fails here.
    """
    heaps = HeapCaps(build_heaps(cfg, sym)[0])
    heaps.registered_heaps_array()
    named = {}
    if sizes.get("JobRunner"):
        named[sizes["JobRunner"]] = "runner"
    if sizes.get("kKeptScratchBytes"):
        named[sizes["kKeptScratchBytes"]] = "scratch"
    out = []
    live: list[tuple[Heap, Block] | None] = []
    with open(record, encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = RECORD_ALLOC.match(raw.strip())
            if m:
                size, caps = int(m.group(1)), int(m.group(2), 16)
                got = heaps.malloc(size, caps)
                if got is None:
                    raise SystemExit("the recording allocates %d bytes (caps 0x%x) and the model "
                                     "has nothing left: the two do not agree" % (size, caps))
                live.append(got)
                name = named.get(size)
                out.append("alloc %-7s 0x%04x%s"
                           % (name or size, caps, "" if name is None else
                              "   # %d bytes in the build it was recorded from" % size))
                continue
            m = RECORD_FREE.match(raw.strip())
            if m:
                ptr = int(m.group(1), 16)
                hit = None
                for i, item in enumerate(live):
                    if item is not None and item[1].ptr == ptr and not item[1].free:
                        hit = i
                        break
                if hit is None:
                    raise SystemExit("the recording frees 0x%08x, which the model never "
                                     "allocated: the two do not agree" % ptr)
                live[hit][0].tlsf.free(live[hit][1])
                live.append(None)
                out.append("free %d" % (hit + 1))
    print("# What ESP-IDF's start-up and this firmware's static initialisers take from the heap")
    print("# before the main task exists - every operation, in order (task p11-01).")
    print("#")
    print("# Recorded from this firmware running in the QEMU of the pinned ESP-IDF image and")
    print("# converted by replaying it over the same build: tools/heap_programme.gdb says how, and")
    print("# tools/heap_at_start.py --from-record is what wrote this file. Re-record it when")
    print("# ESP-IDF, sdkconfig.defaults or a static initialiser of main/ changes.")
    print("#")
    print("#   alloc <bytes|runner|scratch> <caps>   one heap_caps_* allocation")
    print("#   free <n>                              the block operation <n> allocated")
    print("#")
    print("# `runner` is sizeof(mm::JobRunner) and `scratch` is agent::kKeptScratchBytes, read")
    print("# from the ELF of the build being judged: they are the two that change with the build.")
    print("# Recorded from a build of version %s." % version)
    for line in out:
        print(line)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--programme", default=None,
                    help="default: tools/heap_at_start.programme beside this script")
    ap.add_argument("--project-name", default="twinoak-walter")
    ap.add_argument("--min-spare", type=int, default=8192,
                    help="bytes that must be left in the block the main task's stack gets")
    ap.add_argument("--quiet", action="store_true", help="the verdict and the heaps, no op by op")
    ap.add_argument("--from-record",
                    help="a log from tools/heap_programme.gdb: replay it over this build and write"
                         " a programme file to stdout instead of judging the build")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    programme_path = args.programme or os.path.join(here, "heap_at_start.programme")
    build = args.build_dir
    cfg = read_sdkconfig(os.path.join(build, "sdkconfig"))
    sym = read_map_symbols(os.path.join(build, args.project_name + ".map"))
    sizes = read_dwarf_sizes(os.path.join(build, args.project_name + ".elf"))
    if args.from_record:
        return write_programme(args.from_record, cfg, sym, sizes, read_version(build))
    ops = read_programme(programme_path)

    stack_bytes = int(cfg["CONFIG_ESP_MAIN_TASK_STACK_SIZE"])
    tcb_bytes = sizes.get("tskTaskControlBlock")
    if tcb_bytes is None:
        raise SystemExit("no sizeof(TCB_t) in the ELF's DWARF; is this a FreeRTOS build?")
    named = {"runner": sizes.get("JobRunner"), "scratch": sizes.get("kKeptScratchBytes")}

    print("heap_at_start: %s" % build)
    print("  version              %s" % read_version(build))
    print("  _heap_start          0x%08x" % sym["_heap_start"])
    print("  main task stack      %d bytes (CONFIG_ESP_MAIN_TASK_STACK_SIZE), TCB %d bytes"
          % (stack_bytes, tcb_bytes))
    print("  sizeof(mm::JobRunner) %s, agent::kKeptScratchBytes %s"
          % (named["runner"], named["scratch"]))
    print("  programme            %s (%d operations)" % (programme_path, len(ops)))

    heaps, reserved = build_heaps(cfg, sym)
    print("\nthe regions ESP-IDF registers as heaps, after the reserved ranges are trimmed out:")
    for s, e, n in reserved:
        print("    reserved 0x%08x..0x%08x  %s" % (s, e, n))
    print("  %-22s %-10s %-7s %s" % ("heap", "bytes", "free", "capabilities (priority 0/1/2)"))
    caps_order = HeapCaps(heaps)
    for h in caps_order.heaps:
        print("  0x%08x..0x%08x %8d %7s %s" % (
            h.start, h.end, h.size,
            h.tlsf.free_bytes if h.tlsf else "-",
            " / ".join(caps_str(c) for c in h.caps)))
        if h.tlsf is None:
            print("      not registered until main_task runs (heap_caps_enable_nonos_stack_heaps),"
                  " which is after the main task is created: it cannot hold the main task's stack")
    print("  tried in that order, at each of the three priorities"
          " (heap_caps_init.c sorts by increasing size)")

    print("\nwhat is taken before app_main, in the order it is taken:")
    arr_heap, arr_block = caps_order.registered_heaps_array()
    print("    0  alloc %7d %-18s -> 0x%08x in 0x%08x (block %d)  <- heap_caps_init's own array"
          % (HEAP_T_SIZE * len(caps_order.in_region_order), caps_str(CAP_8BIT | CAP_INTERNAL),
             arr_block.ptr, arr_heap.start, arr_block.size))
    allocs: list[tuple[Heap, Block] | None] = []
    for i, (kind, a, b, raw) in enumerate(ops, 1):
        if kind == "alloc":
            size = named.get(a) if isinstance(a, str) else a
            if size is None:
                raise SystemExit("operation %d asks for '%s', which this build has no size for"
                                 % (i, a))
            got = caps_order.malloc(size, b, "op%d" % i)
            allocs.append(got)
            if got is None:
                print("  %3d  alloc %7d %-18s -> NOTHING (the allocator returned NULL)"
                      % (i, size, caps_str(b)))
            elif not args.quiet:
                h, blk = got
                print("  %3d  alloc %7d %-18s -> 0x%08x in 0x%08x (block %d, %d left in the heap)%s"
                      % (i, size, caps_str(b), blk.ptr, h.start, blk.size, h.tlsf.free_bytes,
                         "  <- " + str(a) if isinstance(a, str) else ""))
        else:
            target = allocs[a - 1]
            allocs.append(None)
            if target is None:
                raise SystemExit("operation %d frees operation %d, which allocated nothing" % (i, a))
            h, blk = target
            h.tlsf.free(blk)
            if not args.quiet:
                print("  %3d  free  op %-4d %-18s -> 0x%08x back to 0x%08x (%d free in the heap)"
                      % (i, a, "", blk.ptr, h.start, h.tlsf.free_bytes))

    big = [(i, b) for i, item in enumerate(allocs, 1) if item is not None
           for h, b in [item] if b.size >= 4096]
    tight = None
    if big:
        print("\nthe big ones, and the room each had in the heap it landed in"
              " (the free block it was carved out of, less its own block):")
        for i, b in big:
            print("  %3d  block %6d at 0x%08x, out of a free block of %6d: %6d to spare"
                  % (i, b.size, b.ptr, b.took, b.took - b.size))
        tight = min(b.took - b.size for _, b in big)
        print("  the tightest is %d bytes. %d more bytes of .data or .bss and that allocation"
              " lands in another heap," % (tight, tight + 1))
        print("  which moves everything after it - the main task's stack included. That is what"
              " happened between v0.1.2 and v0.1.3.")

    print("\nthe heap when esp_startup_start_app creates the main task:")
    for h in caps_order.heaps:
        if h.tlsf is None:
            continue
        blocks = [b for b in h.tlsf.blocks]
        print("  0x%08x..0x%08x  %d of %d bytes free, %d block(s), largest free block %d"
              % (h.start, h.end, h.tlsf.free_bytes, h.size, len(blocks), h.tlsf.largest_free()))
        frees = [b for b in blocks if b.free]
        if len(frees) > 1:
            print("      free blocks: %s" % ", ".join(
                "%d at 0x%08x" % (b.size, b.ptr) for b in frees))

    print("\nthe main task (freertos/app_startup.c: xTaskCreatePinnedToCore, then heap_idf.c):")
    where = caps_order.would_fit(stack_bytes, FREERTOS_CAPS)
    if where is None:
        best = max((h.tlsf.largest_free() for h in caps_order.heaps
                    if h.tlsf is not None and (h.all_caps() & FREERTOS_CAPS) == FREERTOS_CAPS),
                   default=0)
        print("  the stack of %d bytes (%s) DOES NOT FIT: no registered heap has a block for it;"
              % (stack_bytes, caps_str(FREERTOS_CAPS)))
        print("  the largest block any of them has is %d bytes." % best)
        print("\nverdict: DOES NOT FIT - xTaskCreatePinnedToCore fails and the boot ends in")
        print("         'assert failed: esp_startup_start_app app_startup.c:86 (res == pdTRUE)'.")
        return 1
    got = caps_order.malloc(stack_bytes, FREERTOS_CAPS, "main stack")
    assert got is not None
    stack_heap, stack_block = got
    tcb = caps_order.malloc(tcb_bytes, FREERTOS_CAPS, "main TCB")
    assert tcb is not None
    tcb_heap, tcb_block = tcb
    print("  the stack of %d bytes -> 0x%08x in heap 0x%08x (block %d bytes)"
          % (stack_bytes, stack_block.ptr, stack_heap.start, stack_block.size))
    print("  the TCB of %d bytes   -> 0x%08x in heap 0x%08x (block %d bytes)"
          % (tcb_bytes, tcb_block.ptr, tcb_heap.start, tcb_block.size))
    spare = stack_heap.tlsf.largest_free()
    print("  left in that heap afterwards: %d bytes, largest free block %d"
          % (stack_heap.tlsf.free_bytes, spare))

    # And what the board itself prints, so that the model can be held to a real start (task
    # p11-02). `app_main`'s first lines print one `heap:` line, and by then `main_task` has
    # registered the region the ROM's stack was in - heap_caps_enable_nonos_stack_heaps(), which
    # runs after the main task exists - so that region's 21 564 bytes are part of the figure and
    # were part of no heap above. The capabilities are the stack's, which is what the firmware
    # asks heap_caps_get_free_size() and heap_caps_get_largest_free_block() for.
    #
    # It is an **upper bound**: the recording ends at esp_startup_start_app (tools/
    # heap_programme.gdb), so whatever main_task and ESP-IDF take between there and app_main's
    # first line is not in it. A board a few hundred bytes below these two is the difference
    # between the breakpoint and the line; a board thousands below, or with a smaller largest
    # block than the stack's own heap had, is a finding about this model.
    for h in caps_order.heaps:
        if h.tlsf is None:
            h.tlsf = Tlsf(h.start, h.size)
    at_app_main = [h for h in caps_order.heaps
                   if (h.all_caps() & FREERTOS_CAPS) == FREERTOS_CAPS]
    print("\nwhat the main task leaves behind, which is where app_main's `heap:` line starts"
          " (docs/bench.md section 1):")
    print("  free %d bytes, largest free block %d, as a request %d (8BIT|INTERNAL, every"
          % (sum(h.tlsf.free_bytes for h in at_app_main),
             max(h.tlsf.largest_free() for h in at_app_main),
             max(h.tlsf.largest_free_request() for h in at_app_main)))
    print("  registered heap - the %d-byte region the ROM's stack was in is one of them by now,"
          " and was not one above)" % next(h.size for h in caps_order.heaps if h.startup_stack))
    print("  **an upper bound, and not by a little**: this is the moment the main task is"
          " created, which is where the recording stops. FreeRTOS's own idle and timer tasks,")
    print("  the task watchdog and app_main's first lines - the USB console among them - take"
          " their share after it, and the board's `heap:` line is that much lower.")
    print("  docs/bench.md section 1 has the figures a board is held to, measured in this"
          " image's QEMU.")

    print("\nverdict: FITS, with %d bytes to spare in the heap the stack came from"
          " (asked for at least %d)" % (spare, args.min_spare))
    # The margin above is the one the rail enforces; this is the one that moves first, and task
    # p11-01's own follow-up asked for it here rather than only in the table above. A build can
    # have 11 kB to spare in the stack's block and still be 357 bytes from a different
    # arrangement - which is what v0.1.2 was, and v0.1.3 is what happened next. A flip is not by
    # itself a boot that fails (the stack would land in the other heap, and this verdict would
    # say so with a much smaller margin), but it is the figure to watch and it is cheap to print.
    if tight is not None:
        print("         the tightest big allocation had %d bytes to spare in the heap it landed"
              " in: that is the figure" % tight)
        print("         that moves first, and one byte less of it rearranges everything after"
              " it (the table above says which)")
    if spare < args.min_spare:
        print("         TOO LITTLE: %d < %d" % (spare, args.min_spare))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
