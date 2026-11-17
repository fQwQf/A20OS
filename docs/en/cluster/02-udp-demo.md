# Real Dual-Core UDP Cluster Acceptance Test

This acceptance test utilizes two independent RISC-V QEMU kernels to implement the actual Native channel proxy and UDP adapter. It goes beyond simply pinging each other; it also treats the loopback virtual nodes on the same kernel not as two separate machines.

## 1. One-Click Execution

```bash
make check-cluster-udp
python3 tools/cluster/check-cluster-udp.py --faults --restart
```

The script automatically generates the configuration `ARCH=riscv64 BRINGUP=1 RAMFS_USER=1 DRIVER_DEPLOYMENT=embedded`, retrieves the QEMU parameters from the Makefile, installs virtio-net, and configures the guest addresses `10.0.3.2/24` and `10.0.3.3/24`. The configuration of the embedded driver is essential: failures caused by unbound network interfaces cannot be attributed to the cluster handshake when only a generic kernel is available without a NIC module.

The host's L2 UDP port is automatically assigned, and the guest's CL port is set to 44020. The default log directory is `.kernel-build/cluster/udp`, which can be overridden using the `--log-dir` option. The `--skip-build` option uses the current build output. The script only cleans up the QEMU processes and sockets it created; it does not terminate other instances.

## 2. Verification Steps

- Two-way communication with 8 unreliable echo requests.
- Two-way communication with 96 reliable echo requests each; the server counts the actual number of times each request is executed to ensure no duplicate executions (count should be 0).
- A shared MCU/host dot operator is used; when a negative number is input, the result is -24.
- Termination of failed requests from unknown services (both normal and reliable), zero-length calls, and calls exceeding the MTU limit is allowed.
- In fault mode, the first request, response, acceptance ACK, and completion receipt are lost, while real online frames are delayed or repeated.
- For calls with all packets lost, the initial request is sent followed by 8 retries; after that, the attempt is canceled, and there should be no permanent waiting.
- After restarting B, A’s old proxy is closed, and a new connection is established successfully. After injecting the old request and old HELLO_ACK, the new service’s execution count remains correct.

The fault injector only forwards or modifies the timing of requests; it does not generate business responses. All business responses come from the guest services running the production code. Successful operations must be logged on both ends; any missing logs or failures will cause the script to exit with a non-zero status. Fault-related actions are also recorded in the `udp-faults.json` file.

## 3. Evidence Explanation

This is evidence of independent kernel functionality and fault recovery in TCG/QEMU, not evidence of the performance of physical MCUs, real switches, wireless serial ports, or large-scale servers. The old `tools/cluster/cluster-net-up.sh` script can still be used for L2 connectivity troubleshooting, but its ping results cannot replace the results of this acceptance test.

The current automatic fixture runs services within kernel threads and does not yet replace the actual operational acceptance of subsequent clusterd/jobd and Native user-space agents. For the complete remaining content, see [../status/current/cluster-implementation-status.md](../status/current/cluster-implementation-status.md).
