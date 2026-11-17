# Minecraft Java Client

The XFCE image for A20OS can run Minecraft Java 1.21.11 on x86_64, aarch64, and riscv64 architectures, using OpenJDK 21, native Wayland windows, and virgl OpenGL. The default desktop entrypoint launches an official demo, which facilitates verifying world generation and interaction in the test image without a logged-in session. Account login, Realms, and online features are not yet available.

## Building and Starting

The host machine must have QEMU virgl support, an available OpenGL display, or an EGL rendering node. For testing, we used q35/KVM with 4 CPUs, 4 GiB of memory, and an AMD Radeon 780M graphics card; a regular 2D virtio GPU is also capable of running XFCE and Firefox, but it is not recommended for use with Minecraft.

```sh
tools/fetch-minecraft.sh 1.21.11 build/minecraft/1.21.11
tools/a20 check minecraft-x86_64
tools/a20 run minecraft-x86_64 -- GUI_MEDIA=build/minecraft/1.21.11
```

The `instances/minecraft-x86_64.toml` configuration uses 4 GiB of memory, a 4 GiB XFCE world, and a Virgl GPU. The download script retrieves the Mojang version list and verifies the client, libraries, and resources using SHA1 checks. The complete dataset consists of 107 libraries, 9 native components, and 4591 asset objects, with a resource index of 29. The files within the image are located in `/usr/share/a20-media/1.21.11`; the client data is not committed to Git.

The desktop image automatically packages the client with native library ELF files that match the architecture of the target system, as specified in the `build/minecraft/*` directory. The `minecraft` executable checks the native libraries again using `uname -m`. If resources are available for multiple architectures, the RISC-V architecture is not selected simply due to directory sorting. The `MC_HOME` environment variable must also match the specified value; the game version is obtained from the `version.json` file. The architecture suffix in the directory name is not included in the version number.

To use this image on an already running desktop, click on the Minecraft menu entry or run the corresponding command.

```sh
minecraft --demo
GAMEDIR="$HOME/.minecraft-test" minecraft --demo --width 854 --height 480
MC_MIN_HEAP=256m MC_MAX_HEAP=1536m minecraft --demo
```

The launcher utilizes the existing `WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR` settings on the desktop, and clears the current process’s `DISPLAY` setting to select the GLFW Wayland backend. It then copies the initial configuration that matches the new game’s requirements: Custom settings, a 4-block field of view, a 5-block simulated distance, with cloud and mipmap features disabled. Existing player configurations will not be overwritten. The Java wrapper simply locates the actual JVM and no longer globally adjusts the heap limit or disables implicit null pointer checks; Minecraft’s heap settings are confined to its own launcher.

## Multi-architecture Clients (aarch64 / riscv64)

Minecraft Java 1.21.11 uses LWJGL 3.3.3 by default. Since Mojang only releases a native version for x86_64 on Linux, other architectures should select the appropriate LWJGL version from the following table. Currently, there is no LWJGL native version available for loongarch64 in any distribution channel, and attempting to download it will result in an error and the exit of the program, without creating a complete client directory.

| Architecture | LWJGL Version | Source | Notes |
| ---- | ---------- | ---- | ---- |
| x86_64 | 3.3.3 | Mojang’s official version list | The original path is used; both the module JARs and the native libraries are downloaded along with the list. |
| aarch64 | 3.3.3 | Maven Central | From the `natives-linux-arm64` repository; this version matches Mojang’s fixed version. |
| riscv64 | 3.3.6 | Maven Central | The riscv64 native libraries have been available since version 3.3.6; all 8 required modules must be of the same version, and the loader enforces this consistency during loading. |

```sh
tools/fetch-minecraft.sh 1.21.11 build/minecraft/1.21.11-x86_64  x86_64
tools/fetch-minecraft.sh 1.21.11 build/minecraft/1.21.11-aarch64 aarch64
tools/fetch-minecraft.sh 1.21.11 build/minecraft/1.21.11-riscv64 riscv64
```

The Maven build process will skip all org/lwjgl entries from the version list (both regular JARs and native JARs are replaced by the Maven versions). After downloading, the SHA1 checksum is verified. When unpacking the native libraries, the ELF magic number and e_machine value are checked (aarch64=183, riscv64=243) to ensure that the correct architecture’s .so files are used and no incorrect versions are included. The client.jar, the remaining libraries, and the 4591 asset files are consistent across all three architectures. The classpath for aarch64/riscv64 includes 67 JAR files (51 original JARs + 16 LWJGL JARs), and 8 LWJGL native .so files are extracted.

The configuration files for the respective instances (`instances/minecraft-{aarch64,riscv64}.toml`) are the same as those for x86_64: 4 GiB of memory, a 4 GiB XFCE world, and a virgl GPU are used. On aarch64, due to SMP (Symmetric Multi-Processing) contention, only a single CPU is used (see [Current Desktop Status](../status/current/desktop-minecraft-status.md); on riscv64, 4 CPUs are used. During startup, the corresponding client directory is passed to `GUI_MEDIA`.

```sh
tools/a20 check minecraft-aarch64
tools/a20 run minecraft-aarch64 -- GUI_MEDIA=build/minecraft/1.21.11-aarch64
tools/a20 run minecraft-riscv64  -- GUI_MEDIA=build/minecraft/1.21.11-riscv64
```

## Verification Method

`tools/tests/xfce-minecraft-smoke.sh` serves as the entry point for objective quality assurance (QA) testing. It only initiates the labwc autostart of a copy of the test image and does not engage in production-level auto-start processes. By default, it uses the separate `/root/.minecraft-qa` directory. The client must be operated manually; the script cannot consider the presence of processes or screenshots of menus as an indication of successful gameplay.

1. Start the writable test image and enter the demo world. Use F3 to verify the virgl, Java, CPU, and coordinate systems.
2. Move the player, open the inventory, then select “Save and Quit to Title,” and finally “Quit Game.”
3. Wait for the script to confirm that the exit code is 0, that the `level.dat` file has been successfully compressed using gzip, and that the SHA256 verification and synchronization processes have been completed.
4. After a normal shutdown, read the archive offline to verify the hash value. Then restart the game from the same disk to re-enter the world and check the player’s position. Do not kill the Java process or terminate QEMU during the world reload.

The regression test for the native GPU path is performed using `make smoke-drm-prime`, which includes the `drm_prime_test` and `gpu3d_test` tests. Changes to GPU/file operations that are common to both ARM32 and AArch64 architectures are only implemented through cross-compilation. These test cases do not represent the behavior of Minecraft on other architectures.
