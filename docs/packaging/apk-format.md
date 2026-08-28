# apk v2 包格式深挖

最后核实：2026-08-27（依据 apk-tools v3.0.7 源码：src/extract_v2.c、
src/tar.c、src/trust.c、src/crypto_openssl.c）。

`mka20pkg.py` 产出的包遵循 apk v2 格式。本文件记录格式的关键约束——
它们都是从 apk 源码逐行核对出来的"隐性规则"，改打包器前请先读这里。

## 总体结构：gzip 分段流

一个 .apk 是**若干段独立的 gzip 流首尾相接**，每段内含一个 tar：

```
[签名段]      tar 流：.SIGN.RSA256.<公钥文件名>     ← 可无（无签名包）
[控制段]      tar 流：.PKGINFO                     ← 包元数据
[数据段]      tar 流：实际文件（含 pax 校验头）
```

### 规则 1：签名段/控制段的 tar 不能以"结束标记"收尾

tar 惯例以两个 512 字节零块表示归档结束，但 apk 的 tar 解析器
（`apk_tar_parse`）见到连续两个零块就**停止解析**，随后若在整个流的
剩余部分（即下一段）读到任何非零块，直接报
`APKE_EOF`（"unexpected end of file"）或
`APKE_FORMAT_INVALID`（"file format is invalid or inconsistent"）。

因此：签名段与控制段的 tar **必须剥掉尾部零块**（abuild-tar 同样不写
结束标记）；数据段保留结束标记，解析器正常收尾。Python 的 tarfile 默认
会写结束标记并把归档填充到 10240 字节——mka20pkg 的
`strip_tar_trailer()` 负责剥除。

### 规则 2：.PKGINFO 必须带 datahash（apk ≥ 2.14，含 3.x）

`datahash = <数据段 gzip 流原始字节的 SHA-256（小写十六进制）>`。

apk 3 在 `apk_pkg_read`（add/index 的共同路径）里要求身份哈希存在；
无 datahash 的包报 `APKE_V2PKG_FORMAT`（"v2 package format error"）。
有了它，数据段完整性由 datahash 独立保证，包签名只需覆盖控制段。

### 规则 3：包签名 = 对控制段 gzip 流的 RSA 签名

- 签名者：`openssl dgst -sha256 -sign key.pem`，输入是**控制段的原始
  gzip 字节**（不是解压后的 tar，也不含数据段）；
- 签名条目名编码算法：`.SIGN.RSA256.<name>` = SHA-256，
  `.SIGN.RSA.<name>` = SHA-1（历史方案）。`<name>` 必须等于公钥在
  信任目录里的**文件名**（如 `a20os-release.rsa.pub`）；
- 验证时机：apk 在控制段→数据段的流边界处验证签名（此时数据段一个字节
  都还没读），数据段交给 datahash 兜底。

### 规则 4：数据段每个普通文件带 pax 校验头

tar 条目带 pax 扩展头 `APK-TOOLS.checksum.SHA1 = <文件内容 sha1 hex>`。
apk 安装时逐文件校验。GNU tar 能读但会警告 "unknown keyword"，无害。

### 规则 5：公钥必须是 SPKI 格式

`apk_pkey_load` 用 OpenSSL 的 `PEM_read_bio_PUBKEY`——接受
`-----BEGIN PUBLIC KEY-----`（SPKI），**不接受** PKCS#1 的
`-----BEGIN RSA PUBLIC KEY-----`。生成方式：

```bash
openssl rsa -in key.rsa -pubout -out key.rsa.pub    # SPKI ✓
```

### 规则 6：仓库路径的架构目录由 apk 自己追加

无论 `-X`/`--repository` 给的是 URL 还是本地路径，apk 都会在后面拼
`/<arch>/APKINDEX.tar.gz`。所以仓库参数永远给**仓库根**，不要手工拼架构。

### 规则 7：相对路径的 --keys-dir 不可靠

apk 内部会 chdir 到目标 root，相对路径的 keys-dir 会静默找不到公钥，
导致整个仓库因索引签名无法验证而被丢弃（症状是诡异的
"no such package"）。`mkrootfs.py` 内部一律绝对化；手工调 apk 时注意。

## 索引（APKINDEX.tar.gz）的签名

`apk index` 产出未签名索引；签名（abuild-sign 的做法）：

1. `openssl dgst -sha256 -sign key APKINDEX.tar.gz` —— 对**整个索引文件**
   签名；
2. 把签名包进一个无结束标记的 tar，gzip 成流；
3. **前置**到 APKINDEX.tar.gz 之前（`cat sig.gz APKINDEX.tar.gz`）。

`tools/apk-sign-stream.py` 实现这一流程，`mka20repo.sh --sign-key`
自动调用。

## 参考验证方法

改动打包器后，用真实 apk 工具验证（这也是本体系开发时的方法）：

```bash
APK=$(tools/ensure-apk-static.sh)
$APK --arch riscv64 --root /tmp/t --initdb --usermode \
     -U --allow-untrusted --force-non-repository add ./your.apk
$APK --keys-dir /abs/path/keys index -o APKINDEX.tar.gz your.apk
```
