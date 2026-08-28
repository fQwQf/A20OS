#!/bin/sh
# smoke-devtools 的 guest 侧脚本：在 chroot（/extra = Alpine 包镜像）内
# 验证上游 gcc 真实可用（编译 + 运行）。通过串口打印 DEVTOOLS_SMOKE: PASS
# 供宿主 grep 判定。TCG 下 cc1/as/ld 加载很慢，宿主侧 timeout 需给足。
export PATH=/bin:/usr/bin:/sbin:/usr/sbin
mkdir -p /tmp || exit 1
/usr/bin/gcc --version || exit 1
echo 'int main(void){return 7;}' > /tmp/hello.c || exit 1
/usr/bin/gcc -O2 -o /tmp/hello /tmp/hello.c || exit 1
/tmp/hello
[ $? -eq 7 ] || exit 1
echo "DEVTOOLS_SMOKE: PASS"
