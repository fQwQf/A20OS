# M0Sense BL702 early probe: factory USB UF2 or JTAG SRAM

This independent M-mode RV32IMC/ILP32 probe does not boot the full A20OS kernel
or userspace. Built without the vendor SDK/libc. Hardware validation is pending.

## Factory USB-C upload (no debugger required)

1. Connect a USB data cable. Hold the software BOOT button (GPIO2), tap RESET,
   and release BOOT after the M0SENSE removable volume appears.
2. Run `python3 deploy-uf2.py --dry-run` to inspect the plan. Then run
   `python3 deploy-uf2.py`, optionally adding `--mount /media/USER/M0SENSE`.
3. The script requires a mounted writable FAT volume and checks INFO_UF2.TXT:
   `Model: Sipeed M0SENSE BL702`, `Board-ID: m0sense-bl702`. It verifies the
   application UF2 and its JSON checksum, then copies only A20PROBE.UF2.
4. The factory loader may detach/reboot automatically. A blue LED heartbeat
   after upload is the probe's visual checkpoint. USB copy success alone is
   not a hardware boot PASS; confirm the UART markers for stronger evidence.

This uses the existing factory UF2 loader and its application partition.
The zero-based UF2 addresses are application-relative. probe-app.bin has an
XIP entry at 0x23000000, exactly as the official Sipeed examples. A small
assembly stub copies code/data to SRAM before any clock changes. probe.uf2
matches Sipeed's uf2_conv.c flags 0x2001, family 0x11de784a and 256-byte payload.
Do not use a generic converter with absolute addresses or raw-flash the app.
The factory loader itself is not included or overwritten by this bundle.

Some board shipments lack the UF2 loader. If BOOT+RESET does not show a volume,
use the ROM recovery procedure in docs/platforms/m0sense-bl702.md. The software
BOOT button is different from the hardware BOOT strap pad used for recovery.

## Optional UART checks

External 3.3 V adapter: GPIO14 TX -> adapter RX, GPIO15 RX -> adapter TX, GND.
Use 115200 8N1. This probe does not implement USB CDC; USB-C is used to upload.

Expected output:

    A20 BL702 M-mode SRAM probe
    MISA=0x........ HART=0x........
    A20 BL702 PROBE READY

It selects RC32M/FCLK, initializes UART0, verifies .data/BSS and a small scratch
array, then handles `i` (CPU information), `m` (RAM PASS) and echo. After checks,
the blue LED on GPIO23 toggles by a polling loop; red/green are off. Interrupts
remain masked. A synchronous exception prints A20 BL702 TRAP and stops.
probe_stage is 1 before UART, 2 after RAM checks, or 0xdead.... after a trap.

## JTAG SRAM alternative

For an already configured/wired external JTAG adapter:

    ./load-probe.sh /absolute/path/to/interface.cfg

This loads probe.elf, with code at 0x22010000 (64 KiB budget) and initialized
data at 0x42020000 (16 KiB including 4 KiB stack), following the SDK RAM map.
It does not erase flash. JTAG pins/pinmux and compatible OpenOCD must be checked
against your PCB revision; USB-C is not a JTAG debugger. Avoid GPIO14/15 JTAG
mapping while the UART is used. probe.bin contains SRAM code only, for inspection;
it is neither a complete RAM upload nor a flash image. Use the ELF for JTAG.

## Artifacts and evidence

- probe.uf2 / probe.json: factory-loader application and format/hash metadata.
- probe-xip.elf / probe-app.bin: XIP-copy application, not ROM boot images.
- probe.elf / probe.bin / probe.map: original nonpersistent SRAM debug route.
- deploy-uf2.py: standalone guarded application uploader (Python stdlib only).

Getting PROBE READY proves early startup, polling UART and limited RAM checks.
The full port still needs M-mode context switching, CLIC/timer support and a
RAM-constrained kernel profile. No scheduler, storage, USB CDC or full OS claimed.

Sources checked 2026-10-25:

- https://wiki.sipeed.com/hardware/en/maixzero/sense/start.html
- https://github.com/sipeed/M0sense_BL702_example/tree/97156083b1ee3a94b7808bfaafd8a28ec78ed466
  (build.sh, misc/utils/uf2_conv.c, libs/uf2_format/uf2_format.h, blink io_def.h)
- https://github.com/bouffalolab/bouffalo_sdk/blob/4b4e4c95449e902206e0d92025c6dc05f3e355d5/drivers/bl702_driver/bl702_flash.ld
- https://github.com/bouffalolab/bouffalo_sdk/blob/1cd1fd514df7ed0dfc1221be871f5aae03f31bbc/bsp/board/bl702dk/bl702_ram.ld
- https://github.com/bouffalolab/bouffalo_sdk/blob/1cd1fd514df7ed0dfc1221be871f5aae03f31bbc/tools/openocd/target/tgt_702.cfg
