# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

@AGENTS.md

## 1. 规则来源

上面一行导入本仓库的 `AGENTS.md`。代码风格、注释、测试、文档写作、信息归属和提交格式都以它为准；本文件只补充 Claude Code 的使用说明。规则要改时，改 `AGENTS.md`，不在这里另存一份。

不要删除这行导入。Claude Code 默认只在没有 `CLAUDE.md` 时加载 `AGENTS.md`；有了本文件，`AGENTS.md` 只能经导入进入上下文。

本仓库作为 gpu_study 的子模块检出在 `4.experiments/gpu-study-qemu` 时，Claude Code 还会逐级向上加载父仓库的 `CLAUDE.md` 和 `AGENTS.md`。两套规则的适用范围按本仓库 `AGENTS.md` 的“文档与交互”一节处理。

`AGENTS.md` 要求的 humanizer-zh 检查，在本机由用户级技能 `humanizer-zh-docs` 执行，用 Skill 工具调用。该技能改编自 op7418/Humanizer-zh，内容与 `AGENTS.md` 固定的上游提交 `f4518a8` 不完全相同；需要严格按上游版本检查时，先告知用户。

## 2. 开始工作前

作为子模块检出时可能处于 detached HEAD。开始修改前先确认分支与工作区，再按 `AGENTS.md` 的分支规则操作。

代码分层、模块划分和协议头位置见 `software/gpgpu/README.md` 第 0 节；依赖、构建与测试命令（含单个 QTest 用例和双进程运行方式）以及当前验证结果见同一文件第 5 节。当前阶段见 `docs/实验进度.md`。

大文件先用锚点或 `rg` 定位，再分段读取：`docs/CP_MEC 第一版规格.md`、`docs/历史/CP_MEC 软件实现、参考与扩展.md` 和 `docs/课程资料/QEMU_2026_讲义.html`。
