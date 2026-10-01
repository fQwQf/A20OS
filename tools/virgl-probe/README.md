# virgl clear-stream probe

Answers one question fast: **does the host renderer actually execute the command
stream A20OS sends, or does it only accept the bytes?**

Run it through `tools/with-virgl-display.sh`, which supplies the host
environment (Mesa as the EGL vendor, the proprietary GPU's DRM node hidden):

```sh
tools/with-virgl-display.sh tools/virgl-probe/run-clear-probe.sh
```

It builds `probe-clear-stream.c` against the headers and library produced by
`tools/build-virglrenderer.sh`, then performs the same calls QEMU performs for
`DRM_IOCTL_VIRTGPU_RESOURCE_CREATE` + `EXECBUFFER`:

    virgl_renderer_init -> context_create -> resource_create -> attach_iov
    -> submit_cmd

with the same 76-byte stream `user/cmds/core/gpu3d_test.c` builds, then prints
the first pixel of the attached backing.

## Why it exists

Through QEMU this question costs a multi-minute boot per attempt, and the guest
can only report that "the host accepted a 76 byte clear stream". The host side
of that is invisible because **QEMU never registers a virglrenderer log
callback**, so `virgl_logv()` returns immediately and vrend's diagnostics --
which name the failing command and the reason -- never appear. Running the same
calls in-process surfaces them.

## Interpreting the result

`RESULT: FAIL untouched` with vrend reporting a context error is the useful
case. vrend latches `ctx->in_error` on the first bad command and every later
command then silently no-ops, so a single wrong length early in the stream
makes the whole frame a no-op while every virtio response still reports success.
That is exactly the shape of "the guest says PASS and the pixels never change".

Two things this has already settled, both of which were wrong before it existed:

- `PIPE_TEXTURE_2D` is **2**. `enum pipe_texture_target` begins with
  `PIPE_BUFFER`, so the 2D entry is the third one, not the second.
- `SET_FRAMEBUFFER_STATE` requires `length == 2 + nr_cbufs`, i.e. 3 for a single
  colour buffer.