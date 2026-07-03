#!/usr/bin/env bash
# Deletes local branches whose upstream tracking branch no longer exists on
# the remote (i.e. it was deleted after a merge). Never touches the branch
# you're currently on.
#
# Usage: scripts/cleanup-branches.sh [remote] [--force]
#   remote   remote to check against (default: origin)
#   --force  use `git branch -D` instead of `-d`, for branches git doesn't
#            consider merged (e.g. squash-merged PRs)
set -euo pipefail

FORCE=false
REMOTE=origin
for arg in "$@"; do
  case "$arg" in
    --force|-f) FORCE=true ;;
    *) REMOTE="$arg" ;;
  esac
done

git fetch --prune "$REMOTE"

gone_branches="$(git branch -vv | awk -v remote="$REMOTE" \
  '$1 == "*" { next } $0 ~ "\\[" remote "/[^]]*: gone\\]" { print $1 }')"

if [ -z "$gone_branches" ]; then
  echo "No stale local branches to clean up."
  exit 0
fi

echo "The following local branches track a deleted $REMOTE branch and will be deleted:"
echo "$gone_branches"
echo

delete_flag="-d"
$FORCE && delete_flag="-D"

echo "$gone_branches" | xargs -r git branch "$delete_flag"
