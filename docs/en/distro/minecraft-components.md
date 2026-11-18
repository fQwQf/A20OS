# Minecraft 1.21.11 Component Status

This table records the operational status of various components in Minecraft 1.21.11 on A20OS, along with evidence of verification. For instructions on how to perform these tests and the actual testing process, see [Minecraft](minecraft.md).

| Component | Status and Evidence |
| --- | --- |
| OpenJDK 21 | Alpine musl 21.0.12; default ergonomics estimate is 1.00 GiB; the client uses up to 1536 MiB based on its own launcher; ImplicitNullChecks are not disabled |
| Client Data | 1.21.11 client.jar, 107 libraries, 9 native binaries, and 29 indices and 4591 assets are complete and verified using the SHA1 checksum |
| Classpath | The launcher overrides the library prefixes one by one using colons, with client.jar added at the end |
| LWJGL / GLFW | Actual backend library version is 3.3.3+5; GLFW supports both Wayland and X11, with the launcher choosing the native Wayland backend |
| Native ABI | musl JVM with gcompat is used to load LWJGL C JNI; it is not guaranteed that any glibc native libraries are compatible |
| OpenAL | Points to the `/usr/lib/libopenal.so.1` musl library to avoid issues with bundled glibc C++ paths; the No Output sound engine started successfully |
| ALSA / HDA | Control/PCM nodes and original ioctl probes are available; however, the alsa-lib reports that the Sound protocol is not compatible, so audio playback has not been successfully implemented |
| jemalloc | The LWJGL system allocator is used; there is no evidence that the bundled glibc jemalloc has been fixed |
| Mesa / virgl | Actual F3 display uses OpenGL 4.3 Core, Mesa 25.2.7, and virgl with AMD Radeon 780M graphics |
| GPU Sharing | PRIME VMO, mmap, SCM_RIGHTS, and exporter lifecycle functions have passed native tests on x86_64/RISC-V platforms |
| GPU Synchronization | The host fence mechanism has been successfully implemented; sync_file has passed tests for polling, merging, details, and dependencies |
| Desktop Composition | labwc uses GLES or pixman depending on 3D_FEATURES; Firefox has successfully verified the Bing interface in both configurations |
| Networking | The Minecraft server returns HTTP 401 responses; a common issue with SIOCGIFCONF has been fixed, and fake tokens are no longer treated as network errors |
| Accounts and Online Status | The official demo is used for quality assurance (QA) purposes; Microsoft login integration is not yet implemented, and Realms/multiplayer features have not been tested |
| World Persistence | The GUI is properly persisted using gzip and sync upon exit; offline reading after shutdown is verified; evidence of cold-start world reloading is available in the Minecraft verification logs |

It cannot be inferred from “original ioctl returning 0” that the entire alsa-lib/OpenAL audio chain is functioning correctly, nor can the final window backend chosen by GLFW be determined from the NEEDED or dlopen lists. The client is accepted based on actual window interactions, F3 functionality, player interactions, and save/load operations. Audio functionality remains an item to be improved.
