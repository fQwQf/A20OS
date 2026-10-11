# Virtgpu transfers, scanout and shared buffers

The native virtgpu transfer path now sends the fixed virtio wire payload in
both directions. RESOURCE_ATTACH_BACKING supplies guest pages; transfer commands
do not carry another entry list. DMA cache maintenance covers the attached VMO.
Zero stride/layer-stride is used for ordinary resources, as required by the
non-blob protocol. The host validates format-dependent boxes and offsets.

Host-rendered framebuffers are presented with SET_SCANOUT and RESOURCE_FLUSH.
The CPU framebuffer path restores its own scanout resource before flushing.
This avoids displaying unchanged guest backing after rendering to a host texture.

SUBMIT_3D requests a host GPU fence and waits for its response. The synchronous
backend exports a real completed sync_file only after that completion. Input
sync_files, poll, FILE_INFO (including details), MERGE and deadline hints are
implemented for completed fences. There is no asynchronous fence producer yet.
A device mutex orders submissions, CPU synchronization and fence export; waiters
are notified when an operation finishes.

PRIME exports hold the live GEM object and map the same VMO, replacing memfd
snapshots and the 64-entry descriptor table. dup and SCM_RIGHTS retain that file
and its object pin. Imported buffers survive the exporter's GEM_CLOSE and fd
close. Mappings retain the VMO after the final descriptor closes. Export flags
use Linux DRM_CLOEXEC=O_CLOEXEC and DRM_RDWR=O_RDWR.

DMA_BUF_IOCTL_SYNC downloads host data on START READ and uploads CPU data on
END WRITE. Sync-file export/import and poll are supported with this synchronous
backend. CPU synchronization currently covers linear level-zero resources;
compressed formats, mip chains and array layouts need further layout tracking.
GEM handles are still device-wide: per-open ownership and namespace accounting
remain separate work. No arbitrary external DMA-BUF exporter is imported.

Device mmap dispatch uses vfile_ops_t.mmap, shared by DRM and DMA-BUF files;
the syscall layer no longer identifies DRM files. Driver ABI 3 rejects modules
built for the old GPU and file operation tables. Rebuild modules with the kernel.

## Verification

Four-CPU preemptive x86_64 and riscv64 guests with host virgl passed:

- native resource allocation and 96 simultaneous objects;
- nonuniform upload, erase guest backing, download and compare;
- real host-rendered red and blue pixels;
- DMA-BUF CPU upload and download against the host resource;
- native fence export, dependency import, poll and FILE_INFO;
- shared mmap writes in both directions, 80 simultaneous exports, SCM_RIGHTS
  across fork, exporter teardown before import, and mappings after fd close;
- sync-file merge, details, deadline hints and invalid argument rejection.

Logs: `build/browser-followup/drm-live-prime-{x86,rv}-guest.log`.
`make smoke-drm-prime` is the durable non-virgl sharing regression.
The old kernel fails the new test when writes to the original mapping do not
appear in the exported mapping; native rendering readback also
rejects a valid zero-stride transfer with EINVAL. Separate matching old modules
were used for those negative checks.
ARM32 and AArch64 kernel builds and the seven architecture boundary assertions
also pass. These are interface tests; Minecraft gameplay is validated separately.

Large requests, backing attachment and fenced submissions now use the same
interrupt-first wait policy as short commands. They recheck the used index after
subscribing, retaining bounded poll/yield for boot or failed IRQ registration.
This removes the fixed 65,536 cache-synchronized polling iterations before a
runtime task can sleep. Native rendering, sharing and synchronization tests pass
on four-CPU preemptive x86_64 and riscv64:
`build/browser-followup/virtgpu-irq-wait-{x86,rv}-guest.log`.
