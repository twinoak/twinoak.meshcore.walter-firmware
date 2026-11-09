# What ESP-IDF's start-up and this firmware's static initialisers take from the heap before the
# main task exists, recorded from the firmware itself (task p11-01).
#
# The pinned ESP-IDF image has QEMU for the esp32s3 and the matching gdb, so this needs no board:
#
#   .\tools\verify.ps1 -Shell
#   idf.py -B /build/fw qemu          # once, to write qemu_flash.bin and qemu_efuse.bin; Ctrl-C
#   qemu-system-xtensa -M esp32s3 -m 32M \
#     -drive file=/build/fw/qemu_flash.bin,if=mtd,format=raw \
#     -drive file=/build/fw/qemu_efuse.bin,if=none,format=raw,id=efuse \
#     -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
#     -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
#     -nic none -gdb tcp::3333 -S -nographic -serial file:/tmp/serial.log &
#   xtensa-esp32s3-elf-gdb -batch -x tools/heap_programme.gdb /build/fw/twinoak-walter.elf \
#     > /tmp/record.log
#   python tools/heap_at_start.py --build-dir /build/fw --from-record /tmp/record.log \
#     > tools/heap_at_start.programme
#
# The last line is also the check: every `F` line's pointer has to be an address the model itself
# allocated, or the conversion fails - so a programme that cannot be replayed is never written.
#
# Two software breakpoints (the heap's code is in IRAM, which gdb can write) and one hardware
# breakpoint for the stop, which is in flash. `heap_caps_aligned_alloc_base` is the one entry
# every heap_caps_malloc/calloc/aligned_alloc goes through (heap/heap_caps_base.c); on entry
# $a2 is the size and $a3 the capabilities. `heap_caps_free` takes the pointer in $a2.
set pagination off
set confirm off
set print pretty off
set print address off
target remote :3333
hbreak esp_startup_start_app
break heap_caps_aligned_alloc_base
commands 2
silent
printf "A size=%u caps=0x%04x\n", $a2, $a3
cont
end
break heap_caps_free
commands 3
silent
if $a2 != 0
  printf "F ptr=0x%08x\n", $a2
end
cont
end
printf "# the heap programme, recorded from reset to xTaskCreatePinnedToCore(main)\n"
continue
printf "# stopped at esp_startup_start_app: the next allocation is the main task's stack\n"
detach
quit
