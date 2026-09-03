"""Instance -> Makefile variable derivation.

Only fields explicitly set in a manifest are emitted as make variables.
Everything else falls through to the Makefile defaults, so policy lives in
exactly one place (the Makefile) and instances carry only their deltas.
"""

from __future__ import annotations

from a20_instance import Instance


def _b(v: bool) -> str:
    return "1" if v else "0"


def derive_make_vars(inst: Instance) -> list[str]:
    """Derive make variable assignments; unset fields keep Makefile defaults."""
    v: list[str] = [f"ARCH={inst.arch}", f"BOARD={inst.board}"]
    k, m, g, n, r, t = inst.kernel, inst.machine, inst.gui, inst.net, inst.rootfs, inst.test
    if inst.abi is not None:
        v.append(f"ABI={inst.abi}")
    if k.profile is not None:
        v.append(f"PROFILE={k.profile}")
    if k.opt is not None:
        v.append(f"OPT={k.opt}")
    if k.user_opt is not None:
        v.append(f"USER_OPT={k.user_opt}")
    if k.bringup is not None:
        v.append(f"BRINGUP={_b(k.bringup)}")
    if k.nommu is not None:
        v.append(f"NOMMU={_b(k.nommu)}")
    if k.driver_deployment is not None:
        v.append(f"DRIVER_DEPLOYMENT={k.driver_deployment}")
    if k.ubsan is not None:
        v.append(f"CONFIG_UBSAN={_b(k.ubsan)}")
    if k.swap is not None:
        v.append(f"CONFIG_SWAP={'y' if k.swap else 'n'}")
    if k.werror is not None:
        v.append(f"KERNEL_WERROR={_b(k.werror)}")
    if k.cooperative_boot is not None:
        v.append(f"COOPERATIVE_BOOT={_b(k.cooperative_boot)}")
    if k.storage_read_only is not None:
        v.append(f"STORAGE_READ_ONLY={_b(k.storage_read_only)}")
    if k.external_root is not None:
        v.append(f"EXTERNAL_ROOT={_b(k.external_root)}")
    if k.ramfs_user is not None:
        v.append(f"RAMFS_USER={_b(k.ramfs_user)}")
    if m.smp is not None:
        v.append(f"NR_CPUS={m.smp}")
    if m.memory is not None:
        v.append(f"QEMU_MEMORY={m.memory}")
    if m.allow_unverified_smp is not None:
        v.append(f"ALLOW_UNVERIFIED_SMP={_b(m.allow_unverified_smp)}")
    if g.display is not None:
        v.append(f"QEMU_GUI_DISPLAY={g.display}")
    if g.audio_driver is not None:
        v.append(f"QEMU_GUI_AUDIO_DRIVER={g.audio_driver}")
    if g.audio_device is not None:
        v.append(f"QEMU_GUI_AUDIO_DEVICE={g.audio_device}")
    if g.frame_window is not None:
        v.append(f"GUI_FRAME_WINDOW={g.frame_window}")
    if n.hostfwd is not None:
        v.append(f"NET_HOSTFWD={','.join(n.hostfwd)}")
    if r.size_mb is not None:
        v.append(f"FAT32_IMAGE_MB={r.size_mb}")
    if r.gui_size_mb is not None:
        v.append(f"GUI_FAT32_IMAGE_MB={r.gui_size_mb}")
    if r.ext4_size_mb is not None:
        v.append(f"EXT4_IMAGE_MB={r.ext4_size_mb}")
    if r.extra_size_mb is not None:
        v.append(f"EXTRA_IMAGE_MB={r.extra_size_mb}")
    if r.world is not None:
        v.append(f"PKG_WORLD={r.world}")
    if r.world_size_mb is not None:
        v.append(f"PKG_SIZE_MB={r.world_size_mb}")
    if r.alpine is not None:
        v.append(f"PKG_ALPINE={_b(r.alpine)}")
    if r.extra_packages is not None:
        v.append(f"EXTRA_PACKAGES={' '.join(r.extra_packages)}")
    if r.drivers is not None:
        v.append(f"DRIVER_SELECTION={' '.join(f'{d}.a20drv' for d in r.drivers)}")
    if t.timeout is not None:
        v.append(f"SMOKE_TIMEOUT={t.timeout}")
    if t.input_delay is not None:
        v.append(f"SMOKE_INPUT_DELAY={t.input_delay}")
    s, fl = inst.stm32, inst.flash
    if s.flash_kb is not None:
        v.append(f"STM32_FLASH_KB={s.flash_kb}")
    if s.ram_kb is not None:
        v.append(f"STM32_RAM_KB={s.ram_kb}")
    if s.xuanwu is not None:
        v.append(f"STM32_XUANWU={_b(s.xuanwu)}")
    if s.qemu is not None:
        v.append(f"STM32_QEMU={_b(s.qemu)}")
    if s.bt_name is not None:
        v.append(f"STM32_BT_NAME={s.bt_name}")
    if s.bt_pin is not None:
        v.append(f"STM32_BT_PIN={s.bt_pin}")
    if s.bt_uuid is not None:
        v.append(f"STM32_BT_UUID={s.bt_uuid}")
    if s.bt_baud is not None:
        v.append(f"STM32_BT_BAUD={s.bt_baud}")
    if s.wifi_ssid is not None:
        v.append(f"STM32_WIFI_SSID={s.wifi_ssid}")
    if s.wifi_password is not None:
        v.append(f"STM32_WIFI_PASSWORD={s.wifi_password}")
    if fl.interface is not None:
        v.append(f"STM32_OPENOCD_INTERFACE={fl.interface}")
    if fl.transport is not None:
        v.append(f"STM32_OPENOCD_TRANSPORT={fl.transport}")
    if fl.adapter_khz is not None:
        v.append(f"STM32_OPENOCD_ADAPTER_KHZ={fl.adapter_khz}")
    if fl.serial is not None:
        v.append(f"STM32_CMSIS_DAP_SERIAL={fl.serial}")
    return v
