# A20 shared-MCU fabric v1

`a20_fabric.sv` implements the synchronous transaction core of the native
shared-memory kernel's hardware adapter. `a20_fsmc_bridge.sv` adds each MCU's
asynchronous port; `a20_shared_ram.sv` provides inferred FPGA RAM and
`a20_shared_top.sv` connects them. This is RTL and firmware integration
simulation evidence; no synthesized FPGA image, PCB or physical MCU run is claimed.

The register contract is in [the kernel design](../../docs/cluster/25-native-shared-kernel.md).
The common software driver is architecture independent; STM32 FSMC is one
bus adapter. See [portable execution domains](../../docs/cluster/27-portable-domains.md)
and [native application integration](../../docs/cluster/26-native-programs.md).
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

The FSMC bridge requires **active-low NWAIT** connected to each MCU's PD6.
The new firmware enables ASYNCWAIT (BCR1/2 = 0x9011) and configures PD6 as an
input. Older fixed-period shared firmware is incompatible with this bridge.
Each MCU has separate CS/strobe/address/data/byte-lane/wait wires; do not join
MCU data output buses together. The target FPGA wrapper must instantiate
appropriate bidirectional I/O cells from `dq_out`, `dq_in`, `dq_oe`.

Strobe activation crosses three synchronizer flops, then bundled payload is
sampled after four settling clocks. The master holds payload stable through
response; payload bits are not individually synchronized. NWAIT asserts
combinationally on CS/strobe activation, deasserts only at registered response,
and DQ is released immediately when the read strobe ends. Keep active strobes
inactive for at least six fabric clocks between operations. Firmware's
conservative 8MHz HSI setup/turnaround covers the logical protocol interval;
physical timing constraints are still required. On a 16-bit FSMC bus A0 is
CPU byte-address bit1; upper address inputs not wired by the 1MiB GPIO map
must be tied low, not left floating.

An aborted request or illegal/changing bus payload latches bridge fault 0x82
and the global fabric fault. A request already accepted by RAM drains once;
it may already have written data. This is a prefix of completed operations,
not rollback. Healthy ports can read FAULT; the broken port holds wait until
domain reset. All domain fault state is sticky.

RAM is two byte-enable arrays intended for inference, with no contents reset.
The default 20 address bits require **1MiB of real FPGA memory capacity**;
the simulation parameter does not prove resource fit on any FPGA. No external
SRAM/SDRAM controller is provided. `RAM_RESPONSE_CYCLES` permits delayed
completion; the bus test uses 300 clocks/3us to exercise waits exceeding the
former fixed DATAST budget. Memory ACK is held until valid withdraws.

Physical integration still requires:

- FPGA I/O wrapper, voltage compatibility, clock/reset constraints, synchronizer
  placement, bundled-payload setup/hold and NWAIT timing analysis. Check RM0008
  asynchronous wait sampling margins, not just logical simulation waveforms.
- A target with enough memory or a separately implemented external RAM backend.
  Synthesis must confirm byte-enable RAM inference, resource fit and timing.
- Reset/power sequencing that preserves sticky attachment through an isolated
  MCU reset and resets the full domain on intended cold boot. The core's
  reset input must not be driven by an individual MCU's reset line.
- FPGA target selection, pin constraints, synthesis/place-and-route, timing
  closure and measured board tests. No pin voltage or asynchronous timing is
  established by this simulation.

Reproducible gates:

- `make check-mcu-shared-fabric`: three-port transaction core, ordering and faults.
- `make check-mcu-shared-bus`: three asynchronous port models through the full
  bridge/core/RAM, including simultaneous requests, held strobes, wait stretching,
  DQ direction, byte lanes, RAM-preserving reset and aborted transaction fencing.
- `make PYTHON=<venv-python> check-mcu-shared-rtl`: real dual Cortex-M3 firmware
  through the same bridge/core/RAM source. Requires ARM GCC, Unicorn 2.1.4,
  pyelftools 0.33, Verilator and a C++ compiler. Every shared RAM access is provided
  by RTL; the instruction engine's mirror never independently supplies read data.
  Fabric register values are also compared with the separate Python oracle.
  This integration harness completes bus transactions sequentially, so concurrent
  in-flight accesses are covered by the bus test, not by this harness.

The integration test uses single-instruction quanta because Unicorn 2.1.4 with
RAM hooks can skip a Thumb instruction at multi-instruction boundaries (also
reproduced with an empty hook). It checks firmware's NWAIT configuration and
executes native scheduling/atomics/heap/migration/exit, plus CPU1 isolated reset
rejection. No physical FSMC electrical behavior, full MCU NVIC model, FPGA
metastability reliability or Minecraft run is established by these tests.
