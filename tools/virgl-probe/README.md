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
case. Registering a log callback is what makes this possible at all: QEMU never
registers one, so `virgl_logv()` returns immediately and vrend says nothing at
all inside QEMU.

`submit_cmd` returning 22 (EINVAL) names the command that was rejected in
`vrend[error]` output. Two failures worth distinguishing, because they look
identical from the guest:

- `Illegal resource N` from a decoder means the resource was never published to
  the context. virglrenderer looks resources up by walking *the context's own*
  table, not a global one, so `virgl_renderer_ctx_attach_resource` is a required
  separate step -- not something resource creation implies.
- A later `Illegal command buffer <garbage>` is the abort path re-reporting the
  next header dword after the first rejection. It is a *consequence*, not an
  independent fault, and its value is not evidence of a misaligned stream.

## What it has settled

- The clear stream `user/cmds/core/gpu3d_test.c` builds is **correct** as
  encoded: 76 bytes, and every length matches virglrenderer 1.3.0
  (`VIRGL_OBJ_SURFACE_SIZE` 5, `SET_FRAMEBUFFER_STATE` `2 + nr_cbufs`,
  `VIRGL_OBJ_CLEAR_SIZE` 8). The earlier "a command length is wrong"
  hypothesis was wrong.
- `virgl_renderer_ctx_attach_resource` is required. Without it: `Illegal
  resource 2` and `submit_cmd -> 22`. With it: `-> 0` and the clear renders.
- Not required, contrary to earlier guesses: `make_current` before
  `resource_create`, and the `submit_cmd(NULL, 0, 0)` flush. Removing either
  still renders.
- Reading the attached iov directly shows the sentinel even when the clear
  worked; `virgl_renderer_transfer_read_iov` returns the rendered pixel. A
  submit alone never writes guest memory, so a readback must ask for a
  transfer.

## Bugs this file itself had

Worth recording, because each one manufactured a false failure that looked
like a kernel bug:

- `VIRGL_FORMAT_B8G8R8A8_UNORM` was passed as 2. It is 1; 2 is
  `VIRGL_FORMAT_B8G8R8X8_UNORM`.
- `PIPE_TEXTURE_2D` was passed as 1. It is 2 -- `enum pipe_texture_target`
  begins with `PIPE_BUFFER`.
- `ctx_attach_resource` was missing, which alone produced
  `Illegal resource 2`.
- No log callback was registered, so the tool could not report the one thing
  it existed to find.
