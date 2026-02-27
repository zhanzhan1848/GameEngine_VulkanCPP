---
description: List all active opencode worktrees
---

List all active worktrees and their branches.

**Steps**

1. **List Worktrees**
   Run:
   ```bash
   git worktree list
   ```
   
   Filter output to show only `.worktrees/` entries if desired.

2. **Check for Unlinked Worktrees**
   Look for worktrees in `.worktrees/` that do not have a corresponding `.openspec` symlink (optional).

3. **Output**
   Display the list of worktrees and their paths.
