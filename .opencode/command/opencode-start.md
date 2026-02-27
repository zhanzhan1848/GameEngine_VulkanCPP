---
description: Start a new feature development environment with Git Worktree and OpenSpec
---

Start a new feature development environment by creating a Git Worktree and linking the OpenSpec.

**Input**: The argument after `/opencode-start` is the change name (kebab-case).

**Steps**

1. **Validate Input**
   If no change name is provided, use **AskUserQuestion** to ask for it.
   > "Which change do you want to start working on?"

2. **Verify Spec Exists**
   Check if `openspec/changes/<name>` directory exists.
   If not, list available changes in `openspec/changes/` and ask user to pick one or exit.

3. **Prepare Worktrees Directory**
   Ensure `.worktrees/` directory exists.
   Ensure `.worktrees/` is in `.gitignore`.

4. **Create Worktree**
   - Worktree path: `.worktrees/<name>`
   - Branch name: `feature/<name>`
   
   Check if worktree path already exists. If so, stop.
   
   Run:
   ```bash
   git worktree add -b feature/<name> .worktrees/<name> HEAD
   ```
   *(If branch already exists, use `git worktree add .worktrees/<name> feature/<name>`)*

5. **Link OpenSpec**
   Create a symbolic link inside the worktree pointing to the spec.
   - Target: `../../openspec/changes/<name>` (relative path recommended)
   - Link: `.worktrees/<name>/openspec/changes`

6. **Copy Opencode Tools**
   Copy the `.opencode` directory into the new worktree so that opencode commands are available there.
   
   Run:
   ```bash
   cp -R .opencode .worktrees/<name>/.opencode
   ```
   *Note: If `.worktrees` is inside `.opencode`, use `rsync` with `--exclude` to avoid recursive copying.*

7. **Output**
   Notify the user that the environment is ready and they can `cd .worktrees/<name>` to start coding.
