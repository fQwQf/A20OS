# ACPI firmware services

`tables.c` and `topology.c` provide the allocation-free early table reader.
They depend only on common integer/string types and caller-supplied physical
mapping callbacks. They contain no architecture headers, virtual-address
conventions or CPU instructions. RSDP checksums, root signatures/entry widths,
SDT lengths/checksums, physical arithmetic and complete MCFG/MADT record bounds
are checked before entries are exposed. The standard MCFG allocation array
starts at byte 44. DSDT is resolved through FADT's DSDT/X_DSDT fields.

A successful table mapping is a borrowed view and must be paired with
`acpi_table_release`. Firmware storage must remain immutable while in use.
Malformed advertised XSDT is rejected instead of silently selecting a different
root. Limits are explicit: RSDP is at most 4096 bytes, an SDT at most 1 MiB.
Architecture callbacks decide which physical spans are accessible during boot;
these bounds do not imply the kernel maps arbitrary firmware physical addresses.

`uacpi_host.c` uses the full pinned uACPI interpreter, not a handwritten AML
subset. After memory and interrupt infrastructure is ready,
`acpi_namespace_initialize` initializes ACPI mode, loads DSDT/SSDT AML, selects
`_PIC` through an architecture-supplied interrupt model, initializes the
namespace and publishes a persistent `fw_node_t` snapshot. Device identity and
status come from evaluated `_HID`, `_CID`, `_UID`, `_ADR` and `_STA`; resources
come from uACPI's decoded `_CRS`. Missing, malformed and unsupported resources
return errors instead of inventing an address or IRQ. The current normalized
node interface exposes fixed memory/I/O and GSI IRQ resources; other ACPI resource
kinds require their own consumer adapter. The full interpreter also evaluates
PCI `_PRT` methods and interrupt-link `_CRS` descriptors and implements the sleep
state protocol. This does not assert that every firmware device/OpRegion type
has an A20OS driver.

The namespace root is the stable provider identity for system GSI references;
IRQ arguments are `[GSI, flags]`, with edge rising/falling = 1/2 and level
high/low = 4/8. `available` requires `_STA` present and enabled;
`children_available` permits traversal when the parent is present or functioning,
including a functioning parent whose present bit is clear. MSFT0101 nodes expose
the native `acpi,tpm2-start-method` value from a validated TPM2 table so drivers
can distinguish TIS from CRB before mapping any register window. Nodes remain
alive for the kernel lifetime. Static resource snapshots do not implement ACPI
hotplug or dynamic `_CRS` refresh.

`uacpi_os.c` supplies kernel allocation, monotonic time, waits, synchronization
and deferred work. Architecture code supplies exact-width port/PCI accesses,
physical table/OpRegion mappings, IRQ installation and interrupt-state changes.
`map_region` is only used for SystemMemory OpRegions; the early table reader
never turns an inaccessible table pointer into a device mapping. The shared
unmap callback must release both mapping kinds. CPU0 deferred work starts after
process initialization; IRQ handlers must never allocate or execute AML methods
inline.

Upstream source and MIT license are in `kernel/external/uacpi`; its
`A20OS-SOURCE` records the fixed commit and downloaded archive SHA256.

Host verification:

```sh
python3 -m unittest discover -s tools/tests -p test_acpi_tables.py
python3 -m unittest discover -s tools/tests -p test_uacpi_namespace.py
```

The first executes the production early reader with malformed/root/topology
fixtures under UBSan. The second links all pinned interpreter sources and the
production node adapter, executes real AML methods/packages/resource templates
and verifies HID/CID/STA/CRS/PRT results, functioning-parent traversal and the
TPM2 protocol property. Its single-thread host service fixtures
are not validation of the kernel OSL, IRQ delivery or physical ACPI hardware.

References:

- [ACPI AML byte encoding](https://uefi.org/specs/ACPI/6.5/20_AML_Specification.html)
- [ACPI device configuration and PCI routing](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/06_Device_Configuration/Device_Configuration.html)
- [uACPI integration and kernel API](https://github.com/uACPI/uACPI/tree/c0edecef62bcac6ce3f783540099190bafadadda)
