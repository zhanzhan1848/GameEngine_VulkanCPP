- 请使用第一性原理思考。你不能总是假设我非常清楚自己想要什么和该怎么得到。请保持审慎，从原始需求和问题出发，如果动机和目标不清晰，停下来和我讨论。如果目标清晰但是路径不是最短，告诉我，并且建议更好的办法

## Workflow Orchestration(工作流编排)

### 1.Plan Mode Default (计划模式优先)
- Enter plan mode for ANy non-trivial task (3+ steps or architectural decisions)
- If something goes sideways, STop and re-plan immediately - don't keep pushing
- Use plan mode for verification steps, not just building- Write detailed specs upfront to reduce ambiguity

### 2.Subagent Strategy (子代理策略)
- Use subagents liberally to keep main context window clean
- Offload research, exploration, and parallel analysis to subagents
- For complex problems, throw more compute at it via subagents
- One task per subagent for focused execution

### 3.Self-Improvement Loop(自我改进循环)
- After ANY correction from the user: update 'tasks/lessons.md with the patternWrite rules for yourself that prevent the same mistake.
- Ruthlessly iterate on these lessons until mistake rate drops
- Review lessons at session start for relevant project

### 4.Verification Before Done (完成前验证)
- Never mark a task complete without proving it worksDiff behavior between main and your changes when relevant
- Ask yourself:"Would a staff engineer approve this?"
- Run tests, check logs, demonstrate correctness

### 5.Demand Elegance (Balanced)(追求优雅(平衡))
- For non-trivial changes: pause and ask "is there a more elegant way?"
- If a fix feels hacky: "Knowing everything I know now, implement the elegant solution"Skip this for simple, obvious fixes - don't over-engineer
- Challenge your own work before presenting it

### 6.Autonomous Bug Fixing (自主修复Bug)
- When given a bug report:just fix it. Don't ask for hand-holding
- Point at logs, errors, failing tests - then resolve them
- Zero context switching required from the user
- Go fix failing CI tests without being told how

## Task Management (任务管理)

1. Plan First: Write plan to 'tasks/todo.md' with checkable items
2. verify Plan:Check in before starting implementation
3. Track Progress: Mark items complete as you go
4. Explain Changes: High-level summary at each step
5. Document Results:Add review section to 'tasks/todo.md'
6. Capture Lessons: Update 'tasks/lessons.md' after corrections

## Core Principles (核心原则)
- Simplicity First: Make every change as simple as possible. Impact minimal code.
- No Laziness: Find root causes. No temporary fixes. Senior developer standards.
- Minimal Impact: Changes should only touch what's necessary. Avoid introducing bugs.
- Core Layer as Capability Provider (核心层只提供能力): 核心层(`Engine/Core`、`Engine/Components`、`Engine/Graphics` 等所有引擎核心模块)统一作为能力提供者,不实现应用逻辑。所有能力通过稳定接口暴露给 UI 层、外部脚本层、外部绑定去组合使用。所有核心模块的设计与开发必须遵循此原则。

## graphify

This project has a graphify knowledge graph at graphify-out/.

Rules:
- Before answering architecture or codebase questions, read graphify-out/GRAPH_REPORT.md for god nodes and community structure
- If graphify-out/wiki/index.md exists, navigate it instead of reading raw files
- After modifying code files in this session, run `graphify update .` to keep the graph current (AST-only, no API cost)
