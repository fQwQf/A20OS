# NAT host-oracle 对照

固定的 C0/Cn/Rust 候选 host 对照注册在[`../../tools/tests/nat-host-comparison.toml`](../../../tools/tests/nat-host-comparison.toml)。它把共享 host oracle 分别对 C 参考源码树与 Rust selector 编译。C0 适配器只把那个源文件抽取到每个全新的运行目录里；它不 checkout 某个版本，也不往工作树写源码。编译之前它向所选编译器询问真实的项目头文件依赖闭包，并拒绝任何字节与固定基线不一致（或在其缺失）的头文件。它在每个 C0 的 `manifest.json` 里记录源码、 fixture、每个被检查头文件的哈希、精确的依赖/编译命令、编译器版本、构建产物哈希与运行输出哈希。头文件一旦变化，就必须改抽历史闭包，而不是让 C0 静默地对着更新的定义编译。

该 oracle 构造 Ethernet/IPv4/TCP/UDP 帧，并在改写之后独立重算 IPv4 与传输层校验和。它把预期契约固定为 44 项具名检查，标识为 `check_names_fnv1a32=3915109928`。注册的用例是纯功能性的：它不把 host 墙钟当作内核性能，也不确立 guest 包路径行为。

在 C/Rust selector 适配器可用之后运行：

```sh
python3 tools/release/v1_compare.py tools/tests/nat-host-comparison.toml \
  --out build/evidence/nat-host-$(date +%Y%m%d-%H%M%S)
```

runner 按带种子的顺序执行两次 C0、两次 Cn 与一次候选运行，把源码/二进制哈希与日志留在新建的输出目录里。功能契约一致时该用例可以报 `PASS`。由于 C0 与 Cn 显式处于未验证状态，聚合结果保持`EVIDENCE_INSUFFICIENT`——在完整基线矩阵被接受之前，这是预期的保守结果。这里不注册也不暗示任何性能标定。
