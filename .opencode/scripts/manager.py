#!/usr/bin/env python3
import os
import sys
import argparse
import subprocess
import shutil

# Configuration
OPENSPEC_DIR = "openspec"
WORKTREES_DIR = ".worktrees"
# CMake build directory name inside the worktree
BUILD_DIR_NAME = "build"

def run_cmd(cmd, cwd=None):
    """Run a shell command and print it."""
    print(f"Running: {cmd}")
    try:
        subprocess.check_call(cmd, shell=True, cwd=cwd)
    except subprocess.CalledProcessError as e:
        print(f"Error executing command: {cmd}")
        sys.exit(e.returncode)

def ensure_worktrees_dir():
    """Ensure worktrees directory exists and is ignored."""
    if not os.path.exists(WORKTREES_DIR):
        os.makedirs(WORKTREES_DIR)
        
    gitignore_path = ".gitignore"
    if os.path.exists(gitignore_path):
        with open(gitignore_path, "r") as f:
            content = f.read()
        if f"{WORKTREES_DIR}/" not in content:
            print(f"Adding {WORKTREES_DIR}/ to .gitignore...")
            with open(gitignore_path, "a") as f:
                f.write(f"\n# Opencode worktrees\n{WORKTREES_DIR}/\n")

def get_worktree_path(feature_name):
    return os.path.join(WORKTREES_DIR, feature_name)

def start_feature(feature_name):
    """Initialize a new feature worktree."""
    spec_path = os.path.join(OPENSPEC_DIR, "changes", feature_name)
    if not os.path.exists(spec_path):
        print(f"Error: Spec not found at {spec_path}")
        print(f"Available specs in {os.path.join(OPENSPEC_DIR, 'changes')}:")
        try:
            for item in os.listdir(os.path.join(OPENSPEC_DIR, "changes")):
                print(f"  - {item}")
        except OSError:
            pass
        sys.exit(1)
    
    ensure_worktrees_dir()
    worktree_path = get_worktree_path(feature_name)
    
    if os.path.exists(worktree_path):
        print(f"Error: Worktree already exists at {worktree_path}")
        sys.exit(1)
        
    branch_name = f"feature/{feature_name}"
    
    # Check if branch exists
    try:
        subprocess.check_call(f"git rev-parse --verify {branch_name}", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        branch_exists = True
        print(f"Branch {branch_name} already exists. Using it.")
    except subprocess.CalledProcessError:
        branch_exists = False
        print(f"Creating new branch {branch_name}...")
        
    if branch_exists:
        run_cmd(f"git worktree add {worktree_path} {branch_name}")
    else:
        run_cmd(f"git worktree add -b {branch_name} {worktree_path} HEAD")
        
    # Link spec to worktree root for easy access
    # Use relative path for symlink to keep it portable
    # spec_path is relative to project root (e.g. openspec/changes/foo)
    # worktree_path is relative to project root (e.g. worktrees/foo)
    # We want link in worktree_path/.openspec -> ../../openspec/changes/foo
    
    abs_spec_path = os.path.abspath(spec_path)
    abs_worktree_path = os.path.abspath(worktree_path)
    rel_spec_path = os.path.relpath(abs_spec_path, abs_worktree_path)
    
    symlink_path = os.path.join(worktree_path, "openspec")
    if os.path.exists(symlink_path):
        os.remove(symlink_path)
    os.symlink(rel_spec_path, symlink_path)
    
    print(f"\n[SUCCESS] Started feature '{feature_name}'")
    print(f"Worktree location: {worktree_path}")
    print(f"Spec linked at:    {worktree_path}/openspec")
    print(f"To start working:  cd {worktree_path}")

def test_feature(feature_name):
    """Build and test the feature."""
    worktree_path = get_worktree_path(feature_name)
    if not os.path.exists(worktree_path):
        print(f"Error: Worktree not found at {worktree_path}")
        sys.exit(1)
        
    print(f"Testing feature '{feature_name}' in {worktree_path}...")
    
    # CMake Build & Test
    build_dir = os.path.join(worktree_path, BUILD_DIR_NAME)
    if not os.path.exists(build_dir):
        os.makedirs(build_dir)
    
    # Configure
    print("Configuring CMake...")
    run_cmd(f"cmake ..", cwd=build_dir)
    
    # Build
    print("Building...")
    run_cmd(f"cmake --build .", cwd=build_dir)
    
    # Test
    print("Running Tests...")
    # Using ctest or custom target 'run_all_tests' as per documentation
    # run_cmd(f"cmake --build . --target run_all_tests", cwd=build_dir)
    # Standard ctest is safer if custom target fails or is not configured
    run_cmd(f"ctest --output-on-failure", cwd=build_dir)
    
    print(f"\n[SUCCESS] All tests passed for '{feature_name}'")

def merge_feature(feature_name):
    """Merge the feature branch into the current branch (usually main)."""
    worktree_path = get_worktree_path(feature_name)
    branch_name = f"feature/{feature_name}"
    
    if not os.path.exists(worktree_path):
        print(f"Error: Worktree not found at {worktree_path}. Cannot merge.")
        sys.exit(1)
    
    # Ensure we are in the main repo context
    # Check current branch
    try:
        current_branch = subprocess.check_output("git branch --show-current", shell=True).decode().strip()
    except:
        current_branch = "unknown"
        
    print(f"Merging '{branch_name}' into '{current_branch}'...")
    
    # Merge
    run_cmd(f"git merge {branch_name}")
    
    print(f"\n[SUCCESS] Merged '{branch_name}' into '{current_branch}'")
    
    # Cleanup prompt
    print(f"You can now remove the worktree with: ./opencode clean {feature_name}")

def clean_feature(feature_name):
    """Remove the worktree and branch."""
    worktree_path = get_worktree_path(feature_name)
    branch_name = f"feature/{feature_name}"
    
    if os.path.exists(worktree_path):
        print(f"Removing worktree {worktree_path}...")
        run_cmd(f"git worktree remove {worktree_path}")
    else:
        print(f"Worktree {worktree_path} does not exist.")
        
    # Ask to delete branch
    print(f"Deleting branch '{branch_name}'...")
    try:
        run_cmd(f"git branch -d {branch_name}")
    except SystemExit:
        print(f"Warning: Failed to delete branch '{branch_name}'. It might not be fully merged.")
        print(f"To force delete, run: git branch -D {branch_name}")
        
    print(f"\n[SUCCESS] Cleanup complete for '{feature_name}'")

def main():
    parser = argparse.ArgumentParser(description="Opencode Workflow Manager")
    subparsers = parser.add_subparsers(dest="command", required=True)
    
    # Start
    start_parser = subparsers.add_parser("start", help="Start a feature (create worktree & link spec)")
    start_parser.add_argument("feature_name", help="Name of the feature (must match openspec/changes/<name>)")
    
    # Test
    test_parser = subparsers.add_parser("test", help="Build and test a feature")
    test_parser.add_argument("feature_name", help="Name of the feature")
    
    # Merge
    merge_parser = subparsers.add_parser("merge", help="Merge a feature branch into current branch")
    merge_parser.add_argument("feature_name", help="Name of the feature")
    
    # Clean
    clean_parser = subparsers.add_parser("clean", help="Remove worktree and delete feature branch")
    clean_parser.add_argument("feature_name", help="Name of the feature")
    
    # Finish (All-in-one)
    finish_parser = subparsers.add_parser("finish", help="Test, Merge, and Clean a feature")
    finish_parser.add_argument("feature_name", help="Name of the feature")
    
    # Status
    subparsers.add_parser("status", help="List active feature worktrees")
    
    args = parser.parse_args()
    
    if args.command == "start":
        start_feature(args.feature_name)
    elif args.command == "test":
        test_feature(args.feature_name)
    elif args.command == "merge":
        merge_feature(args.feature_name)
    elif args.command == "clean":
        clean_feature(args.feature_name)
    elif args.command == "finish":
        test_feature(args.feature_name)
        merge_feature(args.feature_name)
        clean_feature(args.feature_name)
    elif args.command == "status":
        run_cmd("git worktree list")

if __name__ == "__main__":
    main()
