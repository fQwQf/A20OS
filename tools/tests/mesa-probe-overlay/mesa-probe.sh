#!/bin/sh
# mesa-probe —— smoke-mesa-attach 的 guest 侧脚本，在 chroot（/extra = Mesa
# world 镜像）内运行。结果全部打到串口，供宿主 grep。
#
# 它回答的是"stock Mesa 会不会用我们的 virtio-gpu"，而不是"3D 通路通不通"。
# 后者由 smoke-gpu3d-* 用手写命令流证明；前者只有真正加载
# virtio_gpu_dri.so 才算数，而那是 libdrm 的设备枚举 + libEGL/libgbm 的
# DRI device 创建，与前者不是同一条路径。
#
# 输出前缀统一为 MESA_PROBE:，宿主门禁只认这个前缀。

export PATH=/bin:/usr/bin:/sbin:/usr/sbin

echo "MESA_PROBE: begin"

# chroot 之后 /dev 是空的（devfs 填的是宿主根的 /dev，不是 /extra/dev）。
# 没有 /dev/dri，eglinfo 只会报一句含糊的失败，所以这里自己挂。
if [ ! -e /dev/dri ]; then
    mkdir -p /dev 2>/dev/null
    mount -t devtmpfs devtmpfs /dev 2>/dev/null || \
        echo "MESA_PROBE: WARN could not mount devtmpfs"
fi

# 节点与 sysfs 先于 Mesa 检查并报告：没有它们，后面 Mesa 的失败就无法归因。
for n in /dev/dri/card0 /dev/dri/renderD128; do
    if [ -e "$n" ]; then
        echo "MESA_PROBE: node $n present"
    else
        echo "MESA_PROBE: node $n MISSING"
    fi
done

# libdrm 通过 sysfs 枚举设备。DRM class 节点缺失时它会静默地报告"没有设备"，
# 表现出来只是后面一句 "failed to create gbm device"，所以显式检查。
if [ -e /sys/class/drm/card0 ]; then
    echo "MESA_PROBE: sysfs class/drm/card0 present"
else
    echo "MESA_PROBE: sysfs class/drm/card0 MISSING"
fi
if [ -r /sys/class/drm/card0/device/modalias ]; then
    echo "MESA_PROBE: modalias=$(cat /sys/class/drm/card0/device/modalias)"
fi
# 这些属性 libdrm 的 drmGetPciDeviceInfo() 会读；顺序与它的表一致。
for a in vendor device subsystem_vendor subsystem_device revision; do
    if [ -r "/sys/class/drm/card0/device/$a" ]; then
        echo "MESA_PROBE: pci $a=$(cat "/sys/class/drm/card0/device/$a")"
    fi
done

if command -v eglinfo >/dev/null 2>&1; then
    echo "MESA_PROBE: eglinfo present at $(command -v eglinfo)"
else
    echo "MESA_PROBE: FAIL eglinfo NOT INSTALLED"
    echo "MESA_PROBE: end"
    exit 1
fi

# --- libdrm 的设备枚举路径 -------------------------------------------------
# libdrm 的 drmGetDevice2() 是 readdir() 驱动的：它枚举 /sys/class/drm/，再对每个
# 节点 readlink("device")，然后到 /sys/bus/pci/devices/<bdf>/ 读 vendor/device
# 来判定总线类型。缺任何一步，它都拿不到设备身份，Mesa loader 就无法把这个节点
# 映射到任何 DRI 驱动，最后只表现为一句 "eglInitialize failed"。
#
# 所以这里把整条路径逐级列出来，失败时能立刻看出断在哪一级——而不是只看到 eglinfo
# 的那句总结。
echo "MESA_PROBE: --- libdrm enumeration path ---"
for d in /sys/bus /sys/bus/pci /sys/bus/pci/devices /sys/class/drm; do
    if [ -d "$d" ]; then
        echo "MESA_PROBE: dir $d exists"
        # readdir 结果比"存在"更重要：lookup 能命中但 readdir 不列出，libdrm 一样
        # 找不到（这正是 /sys/bus 的情况）。
        listing=$(ls -1 "$d" 2>/dev/null | tr '\n' ' ')
        echo "MESA_PROBE: dir $d lists: ${listing:-<EMPTY>}"
    else
        echo "MESA_PROBE: dir $d MISSING"
    fi
done
# /sys/dev/char/<maj>:<min> 是 libdrm 找设备节点的入口。
if [ -e /sys/dev/char/226:0 ]; then
    echo "MESA_PROBE: char 226:0 -> $(readlink /sys/dev/char/226:0 2>/dev/null)"
    for a in device device/drm device/uevent device/config; do
        if [ -e "/sys/dev/char/226:0/$a" ]; then
            echo "MESA_PROBE: char 226:0/$a exists"
        else
            echo "MESA_PROBE: char 226:0/$a MISSING"
        fi
    done
else
    echo "MESA_PROBE: /sys/dev/char/226:0 MISSING"
fi

# --- GBM：决定 Mesa 是否绑定 virtio-gpu 的那条路 ----------------------------
# MESA_DEBUG=1 让 loader 把"用了哪个驱动"讲清楚。没有它，一个失败只表现为
# "failed to create gbm device"，看不出是找不到设备还是设备被拒绝。
echo "MESA_PROBE: --- eglinfo -p gbm ---"
MESA_DEBUG=1 eglinfo -p gbm 2>&1 | sed 's/^/MESA_PROBE: gbm: /'
echo "MESA_PROBE: gbm-exit=$?"

# --- surfaceless：证明 EGL 本身能初始化，从而把"EGL 坏了"与"没有 DRI 设备"
# 分开。两种失败会长得几乎一样，所以必须分别观察。
echo "MESA_PROBE: --- eglinfo ---"
eglinfo -B 2>&1 | sed 's/^/MESA_PROBE: egl: /'
echo "MESA_PROBE: egl-exit=$?"

# renderer 字符串是判据：virtio_gpu_dri 成功时是 virgl/AMD/Intel 之类，
# 回落到软件时是 llvmpipe/swrast/softpipe。两者必须能区分，否则"Mesa 起来了"
# 会掩盖"用的是 CPU"。
if eglinfo -B 2>&1 | grep -iE 'OpenGL renderer string|renderer string' | sed 's/^/MESA_PROBE: renderer: /'; then
    :
fi

echo "MESA_PROBE: end"