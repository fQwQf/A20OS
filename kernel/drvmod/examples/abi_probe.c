/*
 * Deliberately ABI-mismatched driver package — negative test fixture.
 *
 * A20_DRIVER_DESCRIPTOR takes the ABI as a parameter, so a package can claim
 * an ABI other than the one it was built against without any patching of the
 * emitted ELF.  This one claims A20_DRIVER_ABI + 1.
 *
 * The point is to be rejected.  A module built against a different header
 * hands the kernel a net_dev_ops_t whose trailing fields the kernel would read
 * past the end of, so drvmod_load() refuses on the descriptor's abi field
 * before it resolves DriverEntry (loader.c checks the descriptor first).
 * drvctl and the kernel's own descriptor reader apply the same rule.
 *
 * DriverEntry exists so that this module is well-formed and would load if the
 * check were removed: it announces itself with a marker the gate forbids.  A
 * gate that only asserted "load failed" would also pass if the module were
 * malformed; asserting the marker never appears is what distinguishes a
 * rejected package from a broken one.
 */

#include "drvmod/drvmod.h"

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
                      A20_DRIVER_TYPE_RTC, "abi-probe", A20_DRIVER_ABI + 1,
                      0, 0, 0);

uintptr_t DriverEntry(void)
{
    drv_log("abi-probe: LOADED (this must never appear)\n");
    return 0;
}
