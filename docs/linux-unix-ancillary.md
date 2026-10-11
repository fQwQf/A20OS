# Linux Unix-socket ancillary messages

`SCM_RIGHTS` with `cmsg_len == CMSG_LEN(0)` is a valid empty descriptor
list. Send the data normally, emit no received descriptor header, and continue
parsing subsequent control messages. Retain existing malformed-length and
maximum-descriptor checks.

Firefox's MiniTransceiver always creates an SCM_RIGHTS header, including replies
with no handles. Rejecting that empty list with EINVAL makes the forkserver
crash with `failed to send a reply message`, preventing content-process startup.
This was identified with Firefox 151.0.3's libxul debug symbols at the deliberate
null write in `ForkServer::RunForkServer`.

References:

- [Linux scm_fp_copy](https://github.com/torvalds/linux/blob/master/net/core/scm.c)
- [Firefox MiniTransceiver](https://github.com/mozilla-firefox/firefox/blob/main/ipc/glue/MiniTransceiver.cpp)
- [Firefox ForkServer](https://github.com/mozilla-firefox/firefox/blob/main/ipc/glue/ForkServer.cpp)

`scm_stress` covers empty lists on stream, datagram and sequenced-packet sockets,
and an empty header followed by a nonempty header. The test passes on a Linux
host. The preceding A20OS kernel reports `FAIL empty rights errno=22`; the fixed
4-CPU preemptible x86_64 and RISC-V kernels report `empty-rights PASS` and complete
the descriptor-passing regression. The full x86 VM smoke also completes its
memory, signal and socket probes (85 network-option checks).

Local evidence (generated logs, excluded from source control):

- `build/browser-followup/empty-rights-negative-guest.log`
- `build/browser-followup/empty-rights-full-guest.log`
- `build/browser-followup/empty-rights-rv-guest.log`

This fixes the demonstrated forkserver failure; browser rendering and longer
interactive sessions require separate validation.
