# distro rootfs 是怎么构建出来的

`make rootfs-alpine` 落到 `user/rootfs/alpine/build.sh`。它做的事说起来简单： 用静态的 `apk.static` 把 `packages.txt` 里的几百个包装进一个临时目录，把 `overlay/` 拷进去，最后用 `mkfs.ext4 -d` 直接打成一张 ext4 镜像。麻烦都在细节上， 下面按步骤讲。

## 构建流程

1. **准备 apk 工具**。`apk-tools-static`（一个 .apk 压缩包）从镜像下载并解包， 得到 `sbin/apk.static`。下载和后续的包缓存都落在 `build/cache/alpine/` 下， 第二次构建基本不联网。
2. **装包**。`apk.static --arch <arch> --root <tmp> --cache-packages add $packages` 把 `packages.txt` 里的包装进临时 rootfs。这里有个特性必须知道：**apk 的 post-install 脚本不跑**，所以 gdk-pixbuf 的 loader 缓存、mime 缓存这类本应由 post-install 生成的东西是缺失的，得由 overlay 里的 stage-2 init 在启动时补。
3. **拷 overlay**。`overlay/` 里的内容（stage-2 init、会话脚本、`etc/a20-distro` 标记、udev 规则、labwc 配置等）整体拷进 rootfs；再写 `/etc/apk/repositories` （指向同一个镜像），建 `sbin/init -> /usr/lib/a20/init` 软链，时区指到 `Asia/Shanghai`。
4. **补缓存**。chroot 进去跑 `gdk-pixbuf-query-loaders --update-cache` 和 `update-mime-database`，把上面提到缺的缓存补上（失败可容忍）。
5. **打镜像**。`truncate` 一个 `ROOTFS_SIZE_MB`（默认 8192）大小的文件，然后 `mkfs.ext4 -O ... -d <rootfs>` 直接带目录打进 ext4。所以产物是**没有分区表、 没有 bootloader** 的裸 ext4，宿主上 `mount -o loop` 或直接当 QEMU 根盘。

## 关键文件

- `user/rootfs/alpine/packages.txt`：包清单。基础是 `alpine-base` + bash/coreutils/ busybox/util-linux 那一组，往上叠 XFCE4、labwc、dbus、elogind、polkit、seatd、 eudev、mesa、xterm/xfce4-terminal、xrandr 等，共 439 个包。
- `user/rootfs/alpine/overlay/`：A20OS 塞进发行版的私有文件。`usr/lib/a20/init` 是 stage-2 init（见 boot.md），`usr/lib/a20/start-xfce4-session` 是会话入口， `etc/a20-distro` 是 A20OS init 识别"这是发行版 rootfs"的标记。

## 环境变量

`ARCH`（riscv64/x86_64/aarch64/loongarch64，必填）、`OUTPUT`（产物路径，必填）、 `ROOTFS_SIZE_MB`（>1024）、`ALPINE_MIRROR_ROOT`（默认 `https://mirrors.ustc.edu.cn/alpine`）、`ALPINE_VERSION`（默认 v3.23）。

## 构建环境踩过的坑

- **官方 CDN 慢到像卡死**。容器里 `apk add` 装构建工具时，默认走 dl-cdn.alpinelinux.org，实测十几 KB/s 甚至挂起。解决办法是先把容器的 `/etc/apk/repositories` 指到 USTC，或干脆用下面预置好的镜像。
- **推荐用一个预置 docker 镜像** `a20rootfs-builder:latest`，它预装了 bash/curl/tar/e2fsprogs/coreutils 且 apk 源已指 USTC，一次构建约 2 分钟：

  ```sh
  docker run --rm -v "$PWD:/w" -w /w a20rootfs-builder:latest \
    sh -c 'ARCH=riscv64 OUTPUT=build/alpine/rootfs.img \
           ROOTFS_SIZE_MB=8192 bash user/rootfs/alpine/build.sh'
  ```

一次性制作这个镜像：

  ```sh
  CID=$(docker run -d alpine:latest sleep 3600)
  docker exec $CID sh -c "printf 'https://mirrors.ustc.edu.cn/alpine/latest-stable/main\nhttps://mirrors.ustc.edu.cn/alpine/latest-stable/community\n' > /etc/apk/repositories && apk add --no-cache bash curl tar e2fsprogs coreutils"
  docker commit $CID a20rootfs-builder:latest
  docker rm -f $CID
  ```

- **改了 overlay 必须重建镜像**。镜像 8GiB、由 root 生成，普通用户没法用 debugfs 直接改里面单个文件（ext4 的 metadata_csum 会让 debugfs 报 checksum 错打不开 rw）。任何 overlay 改动都老老实实重跑一次构建。
- **产物权限**。镜像归 root，QEMU 通常以普通用户跑，启动前要 chown 回来 （用 docker `chown 1000:1000` 最省事）。
