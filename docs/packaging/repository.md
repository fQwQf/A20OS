# 包仓库与签名

最后核实：2026-08-27。

## 仓库布局

apk 仓库就是一个静态目录：

```
build/repo/
└── riscv64/                    # 每个架构一个子目录（apk 按 --arch 自取）
    ├── APKINDEX.tar.gz         # 索引（含签名流）
    ├── a20-base-0.2.0-r0.apk
    ├── a20-drivers-0.2.0-r0.apk
    └── a20-kernel-0.2.0-r0.apk
```

建立/更新：

```bash
make pkg-repo ARCH=riscv64
# 等价于：make pkgs → cp 到 build/repo/riscv64/ → tools/mka20repo.sh 建索引
```

`tools/mka20repo.sh` 用 `apk index` 生成 `APKINDEX.tar.gz`；给出
`--sign-key` 时会用 `tools/apk-sign-stream.py` 对索引做 RSA/SHA-256 签名
（与 abuild-sign 相同的做法：对索引文件签名后把签名流前置于 tar 之前）。

## 信任链全貌

```
发布私钥（CI secret A20_REPO_SIGNING_KEY）
   │  签名
   ▼
每个 .apk 包                APKINDEX.tar.gz（仓库索引）
   │                            │
   └── 消费侧用公钥验证 ──────────┘
        公钥目录：Alpine 官方公钥（自动获取）+ A20OS 发布公钥（随仓库发布）
```

- **包签名**：`mka20pkg.py --sign-key` 在包内写入
  `.SIGN.RSA256.<key-name>` 段；`--key-name` 必须与公钥文件名一致
  （apk 按签名条目里的名字在 keys 目录里找公钥）；
- **索引签名**：`mka20repo.sh --sign-key` 自动完成；
- **公钥格式**：SPKI（`-----BEGIN PUBLIC KEY-----`，
  `openssl rsa -in key -pubout` 的输出，**不是** PKCS#1 的
  "BEGIN RSA PUBLIC KEY"）；
- **Alpine 公钥**：mkrootfs 首次需要时从镜像站下载 `alpine-keys` 包
  提取（这是一次信任引导；之后缓存复用）。

## 密钥管理

### 开发密钥（本地）

`make pkg-key`（也是 `make pkgs` 的自动依赖）生成
`build/keys/a20os-dev.rsa{,.pub}`。位于 `build/` 下、被 gitignore，
**仅用于本机闭环验证**，不要把它的公钥分发给用户。

### 发布密钥（项目级）

```bash
# 一次性生成（由维护者保管私钥）
openssl genrsa -out a20os-release.rsa 4096
openssl rsa -in a20os-release.rsa -pubout -out a20os-release.rsa.pub
```

- 私钥 → GitHub 仓库 secret `A20_REPO_SIGNING_KEY`（release workflow
  自动使用）；
- 公钥 → 随 Pages 仓库站点发布（workflow 自动拷贝），用户下载一次放入
  信任目录即可永久验证包与索引；
- 未配置 secret 时，release workflow 仍能运行，但产物**不签名**
  （workflow 会打印醒目警告），消费侧需要 `--allow-untrusted`；
- 私钥泄露 = 立刻吊销：换密钥对、重发索引、公告用户换公钥。

## 发布仓库（GitHub Pages）

release workflow 把 `build/repo/` 部署到 Pages，得到公共仓库：

```
https://<owner>.github.io/<repo>/riscv64/APKINDEX.tar.gz   （+ 各架构）
https://<owner>.github.io/<repo>/a20os-release.rsa.pub     （公钥）
```

在运行 apk 的 A20OS 系统（或任何 apk 环境）里使用：

```bash
# 目标系统上执行一次：信任 A20OS 发布公钥
wget -O /etc/apk/keys/a20os-release.rsa.pub \
    https://<owner>.github.io/<repo>/a20os-release.rsa.pub

# 添加仓库
echo "https://<owner>.github.io/<repo>" >> /etc/apk/repositories

# 之后就是普通的 apk 世界
apk update && apk add a20-base
```

注意 apk 会自己给仓库 URL 追加 `/<arch>/`，所以 `repositories` 里写
站点根即可，不要手写架构目录。

## 本地/内网仓库

仓库是纯静态文件，任何 HTTP 服务器都能托管，甚至可以直接用本地路径
（CI 与 mkrootfs 正是这么用的）：

```bash
tools/mkrootfs.py --arch riscv64 --world my.world --repo /path/to/repo
```
