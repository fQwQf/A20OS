# BSD 软路由的 IPv4 PPPoE

使用 BSD/F-Stack 路由内核。下面以 `en2` 为 LAN、`en3` 为 WAN，
`ppp-wan` 为客户端创建的 TUN。WAN 以太网口保持启用，IPv4 出口
地址和默认路由由拨号客户端建立。

把用户名和密码分别写入凭据文件的第一、第二行，文件由运行客户端的
用户所有，权限设为 `0600`。运行客户端需要管理网卡、路由和原始套接字
的权限。指定运营商要求的认证方式和服务名。服务名为 `-` 时选取首个
有效报价。

```sh
pppoe en3 ppp-wan - chap /etc/pppoe.credentials
```

客户端在前台运行。完成认证和 IPCP 后，它配置 TUN 地址、协商后的 MTU
和主表默认路由，并打印 `PPPOE UP`。已有默认路由会使安装失败，可先
通过网络配置工具移除旧出口。收到终止报文或连续三次 LCP Echo 无应答
时，客户端关闭 TUN 并重新拨号。关闭会释放接口路由和该出口的
MASQUERADE 映射。用 SIGINT 或 SIGTERM 停止。

LAN 源地址的 NAT 仍通过现有规则配置。以下规则用于单台示例客户端：

```sh
echo 'natadd out proto=any src=192.168.10.2 action=masquerade' > /proc/a20/netfilter
```

当前拨号承载 IPv4，使用 PAP 或 CHAP-MD5。IPv6CP 和 PPP 的 DNS 选项
属于后续地址与服务配置工作。发现、会话和 1492 字节上限依据
[RFC 2516](https://www.rfc-editor.org/rfc/rfc2516.html)。
控制和认证协议见 [LCP](https://www.rfc-editor.org/rfc/rfc1661)、
[IPCP](https://www.rfc-editor.org/rfc/rfc1332)、
[PAP](https://www.rfc-editor.org/rfc/rfc1334.html)、
[CHAP](https://www.rfc-editor.org/rfc/rfc1994.html)。

## Linux 对端回归

从本机匹配的 Linux 内核、模块、`pppd`、PPPoE 插件及
`pppoe-server` 制作独立对端 initramfs。工具使用测试账号，无需加载
宿主 PPP 模块或配置宿主网卡。

```sh
python3 tools/net/tests/build-pppoe-peer.py \
  --server <pppoe-server路径> --pppd <pppd路径> --plugin <pppoe.so路径> \
  --kver <Linux内核版本> --legacy-provider <OpenSSL的legacy.so路径> \
  --out <对端构建目录>

make ARCH=x86_64 NET_STACK=bsd NET_ROLE=router NR_CPUS=2 \
  DRIVER_DEPLOYMENT=embedded RAMFS_USER=1 RAMFS_NET_TESTS=1 kernel-only

python3 tools/net/tests/test-bsd-pppoe-kvm.py \
  --kernel <A20OS的kernel.bin> --linux <匹配的Linux内核> \
  --peer-initrd <对端构建目录>/peer-initramfs.cpio.gz --auth pap \
  --out <新的证据目录>
```

将 `--auth` 改为 `chap` 验证 CHAP-MD5。添加 `--silent` 会暂停
Linux 对端并验证 LCP Echo 超时。回归发送经过 NAT 的 LAN UDP 流，
重拨时让对端分配新地址后复用原五元组，并检查错误密码不会建立 TUN。
