# 文档与门禁现状

当前工具行为以 [文档首页](../../README.md)、[测试门禁参考](../../testing-gates.md) 为入口；尚未满足的关闭条件只列在 [DOCS 台账](../gaps/docs.md)。

## 引用与需求候选

`make check-doc-drift` 检查源码路径、需求候选与行号范围，并核对生成的 IDL 头和 Linux syscall 覆盖表。源码已拆分的 NAT、校验和、dcache 与 netfilter 文档引用按当前文件和符号定位，不以过时行号定位。行号检查器仍执行范围校验，没有新增跳过项。

`python3 tools/v1_requirements.py --write` 更新已有候选清单，`--check` 校验路径、分类、候选内容和人工锚点。候选 ID 以文件路径、文本及同文出现次数确定，行号只是阅读定位信息。插入无关代码、移动候选行不要求重生成；改变候选内容、增加/删除同文候选仍会失败。普通 `pending` 标识符不参与待办匹配。生成器不再生成文件内容哈希清单；冻结归档与旧状态叙述归类为历史材料，不作为当前源码需求候选。候选不是任务来源，人工缺口台账仍是唯一待办。

## 冻结归档与用户流程

六份冻结审计 保留原文件和契约标记；静态门禁对标记的依赖继续有效。这些标记只保证历史审计入口存在，归档中的运行结论和内部策略不能作为当前能力证据。

用户路径审查覆盖构建、调试、实例、打包快速开始、发行版构建/引导、翻译器使用、虚拟机使用、集群运维与驱动入门。操作页保留用途、前提、命令、可观察结果与失败语义；驱动教程所需的寄存器/DMA 契约属于开发操作前提。构建夹具 ABI、termios 编码、stamp/实例互斥与 QEMU 参数生成契约集中在 [工具技术参考](../../dev/tooling-internals.md)；ptrace 状态字段、ABI 适配、符号表和 UBSan 接线集中在 [内核调试参考](../../kernel/debugging-internals.md)。翻译器使用页只保留 W^X 策略、验证命令、日志与拒绝语义，字段和生命周期链接到已有[内部契约](../../kernel/exec-xlator/03-internals.md#wx-internals)。操作页与技术页互相链接，命令和行为契约保持一致。

## 网站与 PDF

安装文档站依赖后，从仓库根目录运行：

```sh
python3 -m pip install -r site/requirements.txt
mkdocs build
make check-doc-drift
make docs
```

根配置继承 `site/mkdocs.yml`。发布入口 `bash site/build.sh` 使用同一份配置与链接处理器。两者均开启 strict 模式，缺失目标、无法识别的相对链接和失效锚点都会使构建失败。存在的仓库源码、冻结归档与研究页链接在构建时转为仓库 `main` 文件链接；普通文档链接保留站内检查。没有对应文件的链接继续报错。处理器不修改源 Markdown 或代码块。

`make docs` 调用 `tools/gen_docs_pdf.sh`，需要 pandoc、XeLaTeX、rsvg-convert、fontconfig、 DejaVu 与 Noto CJK 字体；缺少依赖会列出安装命令并退出。生成的参考手册 PDF 不入库。 Pandoc 的警告直接使构建失败。数字开头的中文标题在 MkDocs 与 Pandoc 间使用共同的显式锚点，以保持网页和合并 PDF 的内部跳转一致。

## 验收边界

| 检查 | 当前结论 |
| --- | --- |
| `make check-doc-drift` | 8/8 断言、IDL 与 syscall 覆盖表检查、行号引用检查通过 |
| `mkdocs build` | strict 构建通过；内部链接与锚点无断裂 |
| `make docs` | PDF 编译通过，零警告；封面、目录与正文抽样渲染核对 |
| 文档工具回归 | 需求候选、发布链接、源码引用与清单标记的针对性用例通过 |

`make check-doc-test-gates` 含真实构建和 QEMU smoke。当前实际执行到 RV64 普通/RAMFS 镜像构建时，`kernel/mm/mm.c` 的 `pt_unmap` 因 `idx_path` 可能未初始化而被默认警告策略拒绝，故聚合门禁没有取得 PASS；具体条件见 [MM 台账](../gaps/mm.md)。归档标记的六条断言保持通过；单独执行其关联门禁时，阻塞、信号和超时的其余断言还报告了旧 C 结构与新 Rust 所有权接口之间的漂移，具体请求见 [CORE 台账](../gaps/core.md)。

构建镜像的基础依赖清单及自检均包含 ripgrep；CI 的 `kernel-build` arm32 job 接入了`make check-stm32f103`。在现有固定容器中补齐 ripgrep 后，该命令已运行到固件构建，实际停止在 ARMv7-M Rust target 断言，见 [DRIVER-10](../gaps/drivers.md)。镜像发布与云端 job 未执行；本地复现不计为真实云端运行，也不关闭 DOCS-04。
