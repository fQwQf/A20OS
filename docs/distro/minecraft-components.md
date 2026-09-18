# Minecraft 1.21.11 — per-component readiness

This document audits every component Minecraft needs on A20OS, one at a time, from
the image contents rather than from GUI runs.  It exists because watching the live
desktop turned out to be a poor instrument: a window that never appears cannot tell
you *which* of a dozen libraries is wrong, and several earlier conclusions in
`docs/graphics/3d-graphics.md` were drawn from evidence that could not support them
(see "Method note" below).

Everything here is reproducible from the image with `debugfs`, `readelf` and the
`dlopen` log the LD_PRELOAD shim already produced.

## The root finding: musl JVM, glibc natives

| Side | Evidence |
|---|---|
| JVM is **musl** | `libjvm.so` NEEDED `libc.musl-x86_64.so.1` (Alpine openjdk21) |
| Every LWJGL native is **glibc** | all 9 NEEDED `libc.so.6` + carry `GLIBC_*` versioned symbols |
| Bridge | gcompat: `/lib/libc.so.6 -> libgcompat.so.0`, `/lib/ld-linux-x86-64.so.2` = 22 KB stub |

So every native is a foreign ABI in this process.  gcompat resolves *symbol names*,
which is enough for C, but glibc symbol **versioning is ignored by musl's loader**
and gcompat cannot reproduce glibc's C++ runtime interaction.  That distinction is
the whole story of this file:

* C natives (liblwjgl, liblwjgl_opengl, libglfw, liblwjgl_stb, libfreetype,
  liblwjgl_tinyfd, libjemalloc) — load and run under gcompat.
* C++ natives (libopenal, libjtracy-jni-linux, which need `libstdc++.so.6` +
  `libgcc_s.so.1`) — the C++ exception machinery crosses the musl/glibc boundary and
  faults.  This is the crash that stopped MC before the main menu
  (`al::backend_exception` / `std::system_error` thrown from `libopenal.so+0xf168`).

## What MC actually loads (shim `dlopen` log, across every run)

    loaded:      liblwjgl.so  liblwjgl_opengl.so  libglfw.so  liblwjgl_stb.so  libopenal.so
    never loaded: libjemalloc.so  libjtracy-jni-linux.so  liblwjgl_tinyfd.so

That leaves **exactly one C++ native on the critical path: `libopenal.so`**.
`libjtracy` is gated off in a production client, `libjemalloc` is bypassed by the
launcher, and the rest are C.

## Readiness matrix

| # | Component | State | Evidence / notes |
|---|---|---|---|
| A | Java runtime | OK | Alpine `java-21-openjdk`, musl; MC 1.21 needs 21 |
| B | `/usr/bin/java` wrapper | OK | fixes `$ORIGIN`/libjli resolution + seeds heap |
| C | MC payload | OK | `client.jar` 31 MB, `libraries/` complete, `assets/indexes/29.json`, `natives/`, `version.json` |
| D | Classpath | OK | `classpath.txt` uses `build/minecraft/...` paths; launcher rewrites with `sed s|[^:]*/libraries/|$MC_HOME/libraries/|g` |
| E | LWJGL jars | OK | core/glfw/openal/opengl/stb/tinyfd/jemalloc/freetype 3.3.3 + `-natives-linux` |
| F | Native **files** resolve | OK | every NEEDED of all 9 natives is present in the guest (incl. `ld-linux-x86-64.so.2`) |
| G | Native **ABI** | **BROKEN for C++** | glibc builds in a musl JVM; gcompat can't unwind |
| H | Mesa / GL | OK | musl Mesa 25.2.7: `libGL`, `libEGL`, `libGLESv2`, `libgallium`, `libgbm`; 13 DRI drivers incl. `virtio_gpu_dri.so`, `kms_swrast_dri.so`, `swrast_dri.so`, `zink_dri.so` |
| I | X11 libs | OK | `libX11`, `libxcb*`, `libXrandr`, `libXinerama`, `libXcursor`, `libXi`, `libXxf86vm` |
| J | Wayland libs | OK | `libwayland-client/cursor/egl/server`, `libxkbcommon(+x11)`, `libdecor` |
| K | Compositor | present | `labwc`, `Xwayland`, `libwlroots-0.19.so` |
| L | Audio userspace | partial | alsa-lib, pulse, pipewire, jack present; no sound server runs and the musl openal has no usable backend (see "Audio" below) |
| M | Audio kernel | present | kernel exports `A20OS Audio`/`A20OS PCM`, `alsa_pcm_*`, `a20pcm` |
| N | jemalloc | bypassed | LWJGL's glibc jemalloc faults; `-Dorg.lwjgl.system.allocator=system` |
| O | jtracy | not used | never `dlopen`ed in any run |
| P | Kernel/platform gaps | workarounds in launcher | `-XX:-ImplicitNullChecks` (A20OS SIGSEGV path trips HotSpot's implicit null check), `-Dos.name=Linux` |

## The one component gap that blocks the path, and its fix

`libopenal.so` is the sole C++ native that MC loads, and the image **already
contains a musl build of the same library**: `/usr/lib/libopenal.so.1`
(openal-soft 1.24.3, NEEDED `libc.musl-x86_64.so.1` + `libstdc++.so.6` musl).

LWJGL's OpenAL support is pure dynamic binding — there is no `liblwjgl_openal.so`
in `natives/`, only the OpenAL library itself — so
`-Dorg.lwjgl.openal.libname=...` can point at the musl build directly:

    -Dorg.lwjgl.openal.libname=/usr/lib/libopenal.so.1

That removes the glibc C++ library from the process.  Measured effect: MC no longer
dies.  The run that used to abort with a native C++ throw now gets past it and
reports a Java-level failure instead —

    [Render thread/ERROR]: Error starting SoundSystem. Turning off sounds & music
    java.lang.IllegalStateException: Failed to open OpenAL device

— after which MC keeps going (resource manager reload, unifont, title-screen
requests) with **zero** native aborts.  Sound is off; the process is alive.

### Audio: OpenAL is fine, the kernel PCM does not open through ALSA

The kernel side exposes the device: `/dev/snd/{controlC0,pcmC0D0p,pcmC0D0c}` exist
and the kernel carries `A20OS Audio` / `A20OS PCM` (`alsa_pcm_*`, `a20pcm`).

OpenAL Soft itself is fine.  Asked to log at level 3 it reports its own backends and
where it gives up:

    [ALSOFT] (II) Initializing library v1.24.3-unknown UNKNOWN
    [ALSOFT] (II) Supported backends: pulse, alsa, oss, port, jack, null, wave
    [ALSOFT] (WW) Failed to initialize backend "pulse"
    [ALSOFT] (II) Initialized backend "alsa"
    [ALSOFT] (II) Added "alsa" for playback
    [ALSOFT] (II) Opening playback device "ALSA Default"
    [ALSOFT] (II) Opening device "default"
    [ALSOFT] (WW) Failed to open playback device: Could not open ALSA device "default"

An earlier reading of the dynamic section concluded this build had no working
backend, because the library NEEDs only
`libstdc++.so.6 libgcc_s.so.1 libc.musl-x86_64.so.1`.  That was wrong: this
openal-soft **dlopens its backends**.  It imports `dlopen`/`dlsym` and carries
`AlsaBackendFactory`/`PulseBackendFactory`/`JackBackendFactory`/`NullBackendFactory`
plus `libasound.so.2`, `libpulse.so.0`, `libjack.so.0` as open targets — which is
exactly why none of them shows up as NEEDED.

So the single failing step is `snd_pcm_open("default")`.  Two things about it:

* libasound resolves devices through `/dev/snd/*` (its strings contain
  `/dev/snd/pcmC%iD%ic`, `/dev/snd/pcmC%iD%ip`, `/dev/snd/controlC%i`, ...) and does
  **not** reference `/proc/asound`, so the absence of `/proc/asound` in A20OS is not
  the cause.
* OpenAL Soft does not try another backend once one has initialized, so
  `drivers = alsa, null` still yields no device: alsa initializes, its device open
  fails, and the search stops there.

Two files therefore ship to remove the noise and give applications a device:

* `/etc/asound.conf` points ALSA's `default` at `hw:0,0` instead of the PulseAudio
  plugin chain (which otherwise fails with
  `ALSA lib pulse.c:242:(pulse_connect) PulseAudio: Unable to connect`).
* `/etc/openal/alsoft.conf` selects the null backend, so applications get a real
  device — the "No Output" one — and their audio subsystems initialise quietly
  instead of failing.  Switch `drivers` to `alsa` once the PCM opens.

Verified in a guest run with the configuration installed:

    [Render thread/INFO]: OpenAL initialized on device No Output
    [Render thread/INFO]: Sound engine started

with zero native aborts, i.e. the musl library hands MC a device and its sound
subsystem comes up normally instead of failing and disabling itself.

#### The PCM wire structs in alsa.c do not match the Linux ABI

Compiling the kernel's own structs against the real header
(`/usr/include/sound/asound.h`, same 64-bit ABI) shows the layouts differ:

    struct                 kernel   real    note
    snd_pcm_hw_params         384    608    intervals at offset 100, real 260
    snd_pcm_info              272    288    missing pad1[16]
    snd_interval               16     12    four bitfields pack into 4 bytes

`snd_pcm_hw_params` in the kernel goes `flags; masks[3]; intervals[12]; rmask;
info; ...`, but the real struct is `flags; masks[3]; mres[5]; intervals[12];
ires[9]; rmask; cmask; info; msbits; rate_num; rate_den; fifo_size; ...`.  Every
field after `masks` therefore sits at the wrong offset, `cmask` does not exist at
all, and the `intervals[]` that `alsa_pcm_hw_params` negotiates from and the `info`
it writes are read and written in the wrong place.  That breaks `HW_PARAMS`, which
*is* implemented — the defect is not only in what is missing.

The parameter numbers are not the ABI's either:

    parameter      kernel   real
    FORMAT              0      1
    CHANNELS            1     10
    RATE                3     11
    PERIOD_SIZE        10     13
    PERIODS            11     15

so even with the layout corrected the negotiated fields would name the wrong
parameters.

#### Why the PCM does not open: three ioctls libasound needs are missing

The kernel's ALSA surface is real but narrower than libasound's open path.
`kernel/include/drivers/audio/alsa.h` defines, and `kernel/drivers/audio/alsa.c`
handles:

    PCM:  HW_PARAMS  SW_PARAMS  STATUS  WRITEI_FRAMES  READI_FRAMES
          PREPARE  RESET  START  DROP  HW_FREE  DRAIN  PAUSE  TSTAMP
    CTL:  PVERSION  CARD_INFO  PCM_NEXT_DEVICE  PCM_INFO

Everything else falls through to `default: return -EINVAL`.  What is absent includes

    SNDRV_PCM_IOCTL_PVERSION   0x80044100    (protocol version; SNDRV_PCM_VERSION = 0x20012)
    SNDRV_PCM_IOCTL_INFO       0x81204101    (snd_pcm_info; the size field encodes 288 bytes)
    SNDRV_PCM_IOCTL_HW_REFINE  0xc2604110    (snd_pcm_hw_params; 608 bytes)

and all three sit in libasound's open path before an application sets anything:
`snd_pcm_open` reaches the hw plugin, which asks for the PCM info and then calls
`snd_pcm_hw_params_any()` — HW_REFINE — to learn what the device supports.  With
those returning `-EINVAL`, OpenAL Soft's device open fails exactly where its log
says it does.

Fixing this is kernel work, in this order: put `snd_pcm_hw_params`, `snd_pcm_info` and
`snd_interval` on the Linux layout, use the ABI's parameter numbers, implement
HW_REFINE (report the supported format/rate/channel/period masks and intervals — the
same negotiation `alsa_pcm_hw_params` already performs for the set path), INFO and
PVERSION, then switch `/etc/openal/alsoft.conf` from `drivers = null` to `alsa`.
#### The ABI fix landed, and it was not the whole blocker

The layouts and ioctls above have since been corrected in `alsa.c` (commit
`e29049c0`): `snd_pcm_hw_params` is 608 bytes with `mres[5]`/`ires[9]`/`cmask`/`sync`,
`snd_pcm_info` has `pad1[16]`, `snd_pcm_sw_params` has `proto`, `snd_pcm_status` uses
64-bit timespecs, the parameter numbers are the ABI's, and PVERSION/INFO/HW_REFINE are
implemented.  Every size and offset was checked against the real header (twelve
expectations, all matching) and the kernel builds clean under `-Werror`.

Booting that kernel and asking OpenAL Soft for an ALSA device still fails the same way:

    [ALSOFT] (II) Initialized backend "alsa"
    [ALSOFT] (II) Opening device "default"
    [ALSOFT] (WW) Failed to open playback device: Could not open ALSA device "default"

So the wrong ABI was one defect rather than the whole cause, and the remaining one is
*below* the ioctl layer — the open does not get far enough to be explained by a
rejected request alone, which points at the devfs open path
(`DEVFS_ALSA_PCM` -> `alsa_pcm_create_vfile()`, which fails the whole `open()` if it
returns NULL) or at how the audio device itself is registered.

Nothing was regressed in the meantime: `/etc/openal/alsoft.conf` still selects the null
backend, so Minecraft reaches `OpenAL initialized on device No Output` and
`Sound engine started` with zero native aborts.

The next step is a kernel-side instrument, not another guessing boot: print the request
number in the `default:` branch of the PCM and control dispatch (and a line in the devfs
open path) so one run shows exactly which call fails.  Two other things to settle at the
same time: the QEMU instance passes **no audio device** (`hda`/`virtio-snd` driver
modules exist in `user/build/x86_64/*.a20drv` but nothing binds without a device), while
`pc-spkr` is linked into the kernel and may be the only audio device present — so the
capabilities that device advertises decide whether a PCM can open at all.

#### Root cause: the ALSA layer bound the tone-only PC speaker

Settled statically.  Only one audio device registers in this configuration — the
in-kernel PC speaker — and what it advertises is

    .flags = A20_AUDIO_CAP_TONE

with no `A20_AUDIO_CAP_PCM`.  The two drivers that do advertise PCM are modules
(`hda.a20drv`, `virtio-snd.a20drv`) and they need a matching QEMU device, which this
instance does not pass.

`alsa_audio_get()` made that worse by taking the first audio class device
unconditionally — `class_device_get_by_type(DEV_CLASS_AUDIO, 0)`, which is the PC
speaker — so every PCM path returned `-EOPNOTSUPP`, HW_REFINE among them.  libasound
calls HW_REFINE from `snd_pcm_open()` through `snd_pcm_hw_params_any()`, so the open
failed right there.  That is the whole "Could not open ALSA device" story, and it means
the wrong ABI and the wrong device were two separate defects stacked on each other.

The selection now scans for a device advertising `A20_AUDIO_CAP_PCM`.  All five call
sites are PCM-related (`PCM_NEXT_DEVICE` included), so nothing tone-related changes; a
tone-only system now reports `-ENODEV` for PCM instead of pretending.

Real audio needs an audio device in the VM, and that half is now verified: with
`-device intel-hda -device hda-duplex` on the command line and `hda.a20drv` staged in a
path the driver manager scans (`/bin/lib/drivers`, or `/boot/drivers` — the stock image
also carries it in `/lib/drivers`), the kernel binds it:

    [DRIVER] registered driver 'hda' (class=6)
    [HDA] pci-8086:2668-2 codec=0 afg=1 dac=2 pin=3, 48000 Hz stereo S16_LE
    [DRIVER] device 'pci-8086:2668-2' bound to driver 'hda'

udev then reports two audio class devices (`audio0` and `audio1`) instead of one, which
is exactly the case the selection fix above exists for.  `instances/xfce-x86_64.toml`
now carries the audio device.

Whether ALSA then opens is still unobserved: the run used for the test stalled before
sound initialisation, so no `[ALSOFT]` line appeared.  `drivers = null` therefore stays
in `/etc/openal/alsoft.conf` until a run reaches sound init and reports an ALSA device.
Minecraft is unaffected either way.



### Measured results in the guest

| Check | Result |
|---|---|
| `java -version` | `openjdk version "21.0.12" ... alpine-r0` |
| `/dev/snd` | `controlC0`, `pcmC0D0p`, `pcmC0D0c` present |
| `alcOpenDevice(NULL)` on the musl openal, default driver order | NULL (pulse fails to init; alsa inits but cannot open `default`) |
| MC with musl OpenAL, no openal config | GL init, `Reloading ResourceManager: vanilla`, sound disabled, 0 native aborts |
| MC with musl OpenAL + `/etc/openal/alsoft.conf` | `OpenAL initialized on device No Output`, `Sound engine started`, 0 native aborts |
| MC natives `dlopen`ed | `liblwjgl`, `liblwjgl_opengl`, `liblwjgl_stb`, `libglfw`, `libopenal` — all reached their call sites successfully |
| Compile-on-the-fly Java (`java Foo.java`) | aborts (rc=134) in the in-process compiler — use `javac` then `java -cp` |
| `javac` in the same harness | completed with no message and left no class file, so in-guest JVM tooling needs the launcher's flags too |
| `$JAVA_HOME/bin/java` invoked directly | exits 1 with no output; the `/usr/bin/java` wrapper seeds `-Xms/-Xmx` because the default heap is rejected |
| an in-guest Java harness | must mirror the launcher's flags (`-XX:+UnlockDiagnosticVMOptions -XX:-ImplicitNullChecks`), as MC does |
| `python3` + ctypes | segfaulted once (null deref inside `libpython3.12.so.1.0`, `stval=0x0`) — a new instance of the still-unexplained crash class, not a MC component |


### Long-term options for the glibc-native problem

1. **Build the LWJGL natives for musl** (correct for a musl distro; LWJGL ships only
   glibc Linux natives).  Largest effort, no gcompat in the loop.
2. **Ship a real glibc runtime and a glibc JDK**, running MC outside musl entirely.
   Largest image, removes the bridge rather than shimming it.
3. **Substitute musl system libraries** wherever LWJGL allows a `libname` override
   (OpenAL today; GLFW and FreeType are candidates if the glibc builds misbehave).
   Smallest change, keeps gcompat for LWJGL's own C JNI objects only.

Option 3 is what the launcher does now.

## Method note: `dlopen` evidence is not `NEEDED` evidence

The shim logs `dlopen` calls, which only captures libraries a program *asks for*.
Libraries pulled in as `NEEDED` dependencies are mapped by the dynamic loader and
never appear in that log.  `libGL` NEEDs `libX11.so.6` and `libEGL` NEEDs
`libwayland-client.so.0`, so the presence or absence of those names in a `dlopen`
log says nothing about which windowing backend GLFW chose.  §9.24 of
`docs/graphics/3d-graphics.md` drew a backend conclusion from exactly that
distinction and should be read with this correction in mind.

## Open item

Window presentation (an X11 client creating and mapping a window that is never
composited) is **not** addressed here.  It is a graphics-runtime question and it is
independent of the component audit above: MC's GL path currently completes with no
X window created, so the two problems are separate.
