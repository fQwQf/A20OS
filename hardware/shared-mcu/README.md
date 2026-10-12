# A20 shared-MCU fabric v1

`a20_fabric.sv` implements the synchronous transaction core of the native
shared-memory kernel's hardware adapter. `make check-mcu-shared-fabric` builds
and runs the three-port Verilator testbench. This is RTL simulation evidence;
no synthesized FPGA image, PCB or physical MCU run is claimed.

The register contract is in [the kernel design](../../docs/cluster/25-native-shared-kernel.md).
CPU count is 2–16. Each port has its own hardwired CPU ID (array index). All
requests share a round-robin serialization point; an unfinished RAM write
prevents a later semaphore grant/release from passing it. There are separate
atomic and heap gates. Faults latch until domain reset and fence subsequent
RAM requests. Errors 0x80/0x81 indicate wrong-owner unlock / invalid access.

A requester holds `req_valid`, control/RAM select, write flag, address, data
and byte strobes stable until its `ready` response, then withdraws valid for
at least one fabric clock. `ready` is a response pulse, not an acceptance
queue. Control accesses are 16-bit with both byte lanes enabled. RAM addresses
are byte offsets aligned to a 16-bit word; byte accesses select one lane with
`req_bytes`. A 32-bit MCU access becomes two 16-bit transactions. Software
atomics hold a fabric gate across all component accesses; this interface does
not promise ordinary unprotected multiword accesses are indivisible.

The memory backend holds `mem_ready` until it has completed the active
transaction; its read value is valid when that acknowledgment is sampled.
`mem_valid` and request fields remain stable until acknowledgment. It must
retire each transaction once and handle byte write strobes. Domain reset
clears the protocol state, not external RAM; the boot owner initializes the
shared data/BSS. All ports and any outstanding backend operation must be
quiesced/reset together.

Physical integration still requires:

- An independent FSMC input bridge per MCU: asynchronous strobes and address/
  data capture, clock-domain crossing, stable response until bus completion,
  and correct DQ direction and byte lanes. The GPIO bus adapter currently uses
  fixed conservative timing, without NWAIT. Prove worst-case completion time
  under all active ports or adapt both the board wiring and firmware timing.
- A real SRAM/SDRAM controller and memory capacity/address decoding. The RTL
  core only emits backend transactions; its testbench RAM is not production
  hardware storage.
- Reset/power sequencing that preserves sticky attachment through an isolated
  MCU reset and resets the full domain on intended cold boot. The core's
  reset input must not be driven by an individual MCU's reset line.
- FPGA target selection, pin constraints, synthesis/place-and-route, timing
  closure and measured board tests. No pin voltage or asynchronous timing is
  established by this simulation.

The complete ARM kernel currently runs against a separate instruction-level
fabric model. Connecting that firmware test to this RTL core remains a future
integration gate; passing both independent suites does not prove the physical
FSMC bridge or electrical timing.
