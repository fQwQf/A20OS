# 文档国际化

A20OS 以中文为文档源版本，通过增量翻译维护英文版本。流程参考 [DragonOS 的文档翻译方案](https://dragonos.org/archives/371)：源文档变更、哈希检查、模型翻译、格式验证、翻译 PR、人工审核后合并。

## 目录与站点

中文源文档在 `docs/zh/`，英文译文在 `docs/en/`，两者采用相同相对路径和文件名。例如 `docs/zh/dev/build.md` 对应 `docs/en/dev/build.md`，共同文档 ID 是 `dev/build`。根目录项目简介保留 `README.md` 与 `README.en.md`，使用特殊 ID `@repository-readme`；`docs/README.md` 是语言选择入口，不参与翻译。

```text
docs/
  README.md
  zh/                         # 中文源文档
  en/                         # 英文译文，按需增加
  assets/                     # 共享图片、配置示例等
    localized/zh/             # 含中文文字的图片
    localized/en/             # 对应英文图片
  _meta/
    navigation.yml            # 网站导航：语言无关的文档 ID 和分组 ID
    books/reference.yml       # 参考手册章节顺序
    books/developer.yml       # 开发者手册章节顺序
    books/user.yml            # 用户手册章节顺序
    languages/zh.yml          # 中文导航标签、封面和图表标签
    languages/en.yml          # 英文导航标签、封面和图表标签
  .translation_cache.json     # v2，按文档 ID 索引哈希
tools/docs/pandoc/            # 共用 PDF 模板与过滤器
build/docs/
  site/                       # 官网首页及 docs/ 文档网站
  pdf/zh/                     # 中文 PDF
  pdf/en/                     # 英文 PDF
  reports/                    # 按语言保存的构建报告
```

新增文档时，在中文目录编写内容并把 ID 加入 `docs/_meta/navigation.yml`；分组名称放在两份语言配置的 `sections` 中，中文固定页面标签放在 `pages` 中。已翻译页面的英文导航标题取自英文一级标题，未翻译页使用可读路径并标注 Chinese。导航缺失、无效 ID、重复章节和不存在的章节都会使检查失败，不会回退成扫描目录并拼接全部文件。

MkDocs 的中文 URL 保持不变，例如 `/A20OS/docs/dev/build/`；英文位于 `/A20OS/docs/en/dev/build/`，页面顶部提供语言切换。未翻译页面明确显示中文回退提示；译文过期时展示最新中文。英文页面保留中文源文档的标题 ID，因此跨页章节链接无需随标题翻译修改。

翻译范围为根 README 和 `docs/zh/` 中的 Markdown，排除 `archive/`、`research/`、`paper/`、`locales/`。共享资源、元数据、第三方源码、代码注释和系统运行时消息不参与此翻译流程。文档内部使用相对链接；指向仓库代码的路径需要从语言目录向上多退一级。网站会将未发布的源码、归档和研究文档链接转换为仓库地址。

## PDF 构建

先安装 `site/requirements.txt` 中的 Python 依赖，以及 Pandoc、XeLaTeX、rsvg-convert、DejaVu 和 Noto CJK 字体。`make docs` 会检查系统依赖并给出缺失项安装提示。网站与 PDF 构建均不调用翻译 API；PDF 图片必须是仓库内资源。

```bash
make docs
make docs DOC_LANG=en
make docs DOC_LANG=all DOC_BOOK=developer
make docs DOC_LANG=en DOC_BOOK=user DOC_MODE=preview
python3 tools/docs/build_pdf.py --lang en --book reference --plan
```

`DOC_LANG` 可取 `zh`、`en`、`all`，`DOC_BOOK` 默认为 `reference`，也提供 `developer` 与 `user`。参考手册精选架构、内核、ABI 和各子系统核心章节；开发者手册集中于构建、贡献、调试和驱动开发；用户手册集中于运行、实例、打包及运维。全部在线文档仍由网站导航覆盖。修改相应 `books/*.yml` 可调整章节顺序与范围，不复制正文。

默认 `DOC_MODE=strict`：英文手册逐章检查源文、译文与翻译策略哈希，存在缺失或过期译文时列出清单并退出。`DOC_LANG=all` 在生成任何新 PDF 前检查两种语言。`DOC_MODE=preview` 允许中文回退，封面标记 PREVIEW、每个回退章节标注 CHINESE FALLBACK，文件名带 `-preview`，不会覆盖正式版。

正式产物为 `build/docs/pdf/<lang>/<book>.pdf`，预览为 `<book>-preview.pdf`。默认中文参考手册同时复制到根目录 `standard-reference.pdf`。封面、目录、章节及图表标签按语言配置生成；两种语言均保留 CJK 字体，支持源代码中的中文注释。各文档锚点在 PDF 中具有独立命名空间：本书内的链接转为 PDF 锚点，未收录页面的链接转为相应语言的在线页面；图片按原文档位置解析。

`build/docs/reports/<lang>/<book>[-preview].json` 记录实际章节、回退状态、源文和所选内容哈希、模板和清单哈希、Git 提交、工作区修改状态、工具版本及 PDF 哈希。构建日期可通过 `SOURCE_DATE_EPOCH` 固定。构建失败不会把不完整 PDF 发布为正式产物。

## 配置自动翻译

在 GitHub 仓库 Settings 的 Secrets and variables → Actions 中设置：

| 类型 | 名称 | 内容 |
| --- | --- | --- |
| Secret | `DOC_TRANSLATION_API_KEY` | 模型服务的 API 密钥 |
| Variable | `DOC_TRANSLATION_BASE_URL` | HTTPS 的 OpenAI 兼容 API 基址，包含服务要求的版本路径；不含 `/chat/completions` |
| Variable | `DOC_TRANSLATION_MODEL` | 服务支持的模型标识 |
| Variable | `DOC_TRANSLATION_BATCH_SIZE` | 可选，每批文件数，默认 20；设为 0 处理全部待翻译文件 |

在 Actions 的 Workflow permissions 中允许工作流创建 Pull Request。未配置模型时，翻译工作流给出提示并跳过；文档检查和站点发布仍可运行。不固定供应商，也不假定任何模型永久免费。

`main` 上的文档修改会触发翻译；首次可手动运行 Translate documentation。每批创建 `codex/docs-translation` 分支的 PR，已有待审 PR 时跳过，避免重复计费。合并后继续处理剩余文件；工作流不会自动合并。不要直接向该自动化分支提交其他改动。由默认 `GITHUB_TOKEN` 创建的 PR 不会触发新的 PR 工作流，因此翻译工作流在创建 PR 前自行执行测试和双语构建；仓库若要求独立 PR 检查，需维护者触发检查或另行配置 GitHub App token。

## 本地操作

以下只检查状态，不访问网络：

```bash
python3 tools/docs/doc_translator.py --dry-run
python3 tools/docs/doc_translator.py --check
python3 tools/docs/doc_translator.py --check-current
python3 tools/docs/doc_translator.py --prune
```

`--check` 允许存在待翻译或待更新文件，检查已有译文与缓存的完整性；`--check-current` 要求所选范围全部同步，适合发布验收。设置与 CI 同名的三个模型环境变量后，可执行：

```bash
python3 tools/docs/doc_translator.py --limit 20
python3 tools/docs/doc_translator.py --files dev/build
```

CI 从环境变量读取密钥，GitHub Actions 使用加密 Secret。本地也可把上述三个同名设置保存为仓库外的 `~/.config/a20os/doc-translation.json` JSON 对象（设置了 `XDG_CONFIG_HOME` 时使用该目录），目录权限设为 `0700`，文件权限设为 `0600`。翻译器仅在实际需要调用模型时读取私有配置；不接受仓库内的密钥文件、符号链接或其他用户可读的配置。环境变量优先，但必须三个一起设置，避免把本地密钥发送到意外覆盖的接口。密钥不写入仓库文件、缓存或日志。

硅基流动的免费模型配置可使用 `DOC_TRANSLATION_BASE_URL=https://api.siliconflow.cn/v1` 和 `DOC_TRANSLATION_MODEL=tencent/Hunyuan-MT-7B`。截至 2026-11-08，[官方价格页](https://www.siliconflow.cn/pricing)将该模型输入和输出均列为免费；使用前仍应核对最新价格。需要已实名认证账号的 API Key，仍受速率限制。翻译器不会自动切换到付费模型。硅基流动请求显式设置 `max_tokens=8192`，并检查输出是否被截断。

服务收到的是带占位符的文档正文和术语表。缓存 `docs/.translation_cache.json` 随译文提交，记录源文档、译文与翻译规则的哈希，以及模型标识。修改术语表或提示词会使缓存失效；切换模型不会自动重翻全部文档，需要时显式使用 `--force`。

翻译器保护代码围栏、行内代码、链接目标、HTML 标签、显式锚点和数值。可以用 `<!-- i18n:keep-start -->` 与 `<!-- i18n:keep-end -->` 包住需要原样保留的内容。代码块内的中文注释也会保留，便于命令逐字核验。模型丢失占位符、改变标题层级或截断输出时不会覆盖旧译文；API 限流（429）和临时服务错误最多尝试三次（含首次请求），采用 10/20 秒指数退避并加入随机抖动；服务端 `Retry-After`（秒数或 HTTP 日期）优先作为最短等待时间。402 视为账户余额/支付错误，立即停止，不循环重试。成功文件逐个保存，失败后可重跑，已经成功的文件会跳过。

源文件删除后，仅清理缓存登记且未被手工改动的对应译文。删除文档的 PR 可先运行 `--prune` 离线清理旧译文，避免孤立文件触发门禁。重命名按删除旧文件和新增文件处理。未登记或被手工修改的译文不会被自动覆盖。

## 审核与维护

审核重点是术语、数值对应的含义、前提条件以及“计划”“部分实现”“已验证”等状态。格式检查不能证明翻译语义正确。术语统一在 `tools/docs/i18n/glossary.json` 中维护。

手工修订译文后，确认它与当前中文源文档完整对应，再登记新的哈希：

```bash
python3 tools/docs/doc_translator.py --adopt --files dev/build
make check-docs
python3 -m unittest tools.tests.test_doc_translator
python3 -m pip install -r site/requirements.txt
bash site/build.sh
```

`--adopt` 不执行翻译，也不会证明语义正确；它只验证受保护内容并登记审核后的版本。不要用它掩盖尚未同步的中文改动。目录迁移已保留此前审核过的译文及哈希状态；后续正文修改仍按增量流程同步。

### 翻译请求限速

翻译器串行请求，默认请求开始时间至少间隔 6 秒（约 10 RPM），重试也计入限速。可通过环境变量调整，例如 `DOC_TRANSLATION_MIN_INTERVAL=12 python3 tools/docs/doc_translator.py --limit 1`。该值必须是有限正数，单位为秒；它是本项目的保守默认值，不是平台公布的免费额度。限制在单个翻译进程内生效，请勿同时启动多个批量翻译进程。

根据[官方限流政策](https://docs.siliconflow.cn/docs/userguide/faqs/rate-limit-and-upgradation)，实名认证后可使用免费模型，免费调用账单为零；免费模型额度固定，限额按账户及模型计算，多个 API Key 不会增加同一账户额度。RPM 和 TPM 任一超限均会触发限制，单纯限制请求频率不能保证不触发 TPM；遇到 429 时由退避处理，并应按控制台实际额度增加请求间隔。不要将通用模型配额表直接当作免费模型配额。
