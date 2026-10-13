"""Synthetic firmware DTB for the QEMU BCM2711 gate, not a hardware DTB.

No dtc dependency: encode the small v17 fixture explicitly. Production code
consumes a firmware-provided standard DTB through core/fdt_view.c.
"""
import struct


def word(value):
    return struct.pack(">I", value)


class Tree:
    def __init__(self):
        self.structure = bytearray()
        self.strings = bytearray()
        self.offsets = {}

    def align(self):
        self.structure.extend(b"\0" * (-len(self.structure) % 4))

    def begin(self, name):
        self.structure.extend(word(1) + name.encode() + b"\0")
        self.align()

    def end(self):
        self.structure.extend(word(2))

    def prop(self, name, data):
        if name not in self.offsets:
            self.offsets[name] = len(self.strings)
            self.strings.extend(name.encode() + b"\0")
        self.structure.extend(word(3) + word(len(data)) + word(self.offsets[name]) + data)
        self.align()

    def text(self, name, value):
        self.prop(name, value.encode() + b"\0")

    def cells(self, name, *values):
        self.prop(name, b"".join(word(value) for value in values))

    def finish(self, reservations):
        self.structure.extend(word(9))
        reserve = b"".join(struct.pack(">QQ", *entry) for entry in reservations)
        reserve += bytes(16)
        structure_offset = 40 + len(reserve)
        strings_offset = structure_offset + len(self.structure)
        total = strings_offset + len(self.strings)
        header = struct.pack(">10I", 0xd00dfeed, total, structure_offset,
                             strings_offset, 40, 17, 16, 0, len(self.strings),
                             len(self.structure))
        return header + reserve + self.structure + self.strings


def board_dtb(order=(0, 1, 2, 3), disabled=(), reservations=((0, 0x1000),),
              memory=0x3c000000, method="spin-table", release_delta=0,
              compatible="raspberrypi,4-model-b\0brcm,bcm2711",
              bootargs="a20.hostname=rpi4-test a20.smp_test=1", reserved_memory=None):
    tree = Tree()
    tree.begin("")
    tree.text("compatible", compatible)
    tree.cells("#address-cells", 2)
    tree.cells("#size-cells", 2)
    tree.begin("memory@0")
    tree.text("device_type", "memory")
    tree.cells("reg", 0, 0, 0, memory)
    tree.end()
    tree.begin("chosen")
    tree.text("bootargs", bootargs)
    tree.end()
    if reserved_memory:
        tree.begin("reserved-memory")
        tree.cells("#address-cells", 2)
        tree.cells("#size-cells", 2)
        tree.prop("ranges", b"")
        tree.begin("test@%x" % reserved_memory[0])
        tree.cells("reg", 0, reserved_memory[0], 0, reserved_memory[1])
        tree.prop("no-map", b"")
        tree.end()
        tree.end()
    tree.begin("cpus")
    tree.cells("#address-cells", 1)
    tree.cells("#size-cells", 0)
    for cpu in order:
        tree.begin("cpu@%x" % cpu)
        tree.text("device_type", "cpu")
        tree.text("compatible", "arm,cortex-a72")
        tree.cells("reg", cpu)
        tree.text("enable-method", method)
        tree.cells("cpu-release-addr", 0, 0xd8 + cpu * 8 + release_delta)
        if cpu in disabled:
            tree.text("status", "disabled")
        tree.end()
    tree.end()
    tree.end()
    return tree.finish(reservations)


if __name__ == "__main__":
    import argparse
    from pathlib import Path
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(board_dtb(bootargs="a20.hostname=rpi4-qemu" +
                                     (" a20.smp_test=1" if args.selftest else "")))
