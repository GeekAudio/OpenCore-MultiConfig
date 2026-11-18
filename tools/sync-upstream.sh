#!/bin/bash
#
# Replay the OCMC changes onto a new upstream OpenCorePkg release.
#
# Usage:
#   ./tools/sync-upstream.sh 1.1.0
#
# What it does:
#   1. fetches upstream tags into 源码/ocmc-git
#   2. tags the fetched release as upstream-<ver>
#   3. creates branch sync-<ver> from it and cherry-picks the latest ocmc-* commit
#   4. on success, tags ocmc-<ver> and writes patches/ocmc-<ver>.patch
#
# On conflict it stops, lists the files and prints the exact commands to finish.
# Re-running the script afterwards resumes from the existing branch.
#
# The source repository layout (源码/ocmc-git):
#   tag upstream-1.0.8   pristine OpenCorePkg 1.0.8
#   tag ocmc-1.0.8       the same tree plus the OCMC changes
#
set -euo pipefail

NEW="${1:-}"
if [ -z "${NEW}" ]; then
  echo "usage: $0 <upstream-version>   e.g. $0 1.1.0"
  echo "       (upstream tags: https://github.com/acidanthera/OpenCorePkg/tags)"
  exit 2
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "${HERE}/../../源码/ocmc-git" 2>/dev/null && pwd)" || {
  echo "error: source repository not found at 源码/ocmc-git" >&2
  exit 1
}

cd "${REPO}"

git remote get-url upstream >/dev/null 2>&1 || \
  git remote add upstream https://github.com/acidanthera/OpenCorePkg.git

echo "==> fetching upstream tags"
git fetch upstream --tags --force

if ! git rev-parse -q --verify "refs/tags/${NEW}" >/dev/null; then
  echo "error: upstream tag ${NEW} not found after fetch" >&2
  exit 1
fi

BASE="upstream-${NEW}"
NEWTAG="ocmc-${NEW}"
BRANCH="sync-${NEW}"

LAST="$(git tag -l 'ocmc-*' --sort=-v:refname | head -1)"
if [ -z "${LAST}" ]; then
  echo "error: no ocmc-* tag found to replay" >&2
  exit 1
fi
echo "==> base tag ${BASE} (from upstream ${NEW}), replaying ${LAST}"

git tag -f "${BASE}" "refs/tags/${NEW}" >/dev/null

if git rev-parse -q --verify "refs/heads/${BRANCH}" >/dev/null; then
  echo "==> resuming existing branch ${BRANCH}"
  git checkout "${BRANCH}"
  if [ -e ".git/CHERRY_PICK_HEAD" ]; then
    echo "error: cherry-pick still in progress. Resolve and continue first:" >&2
    echo "  cd \"${REPO}\" && git status" >&2
    echo "  git add -A && git cherry-pick --continue" >&2
    exit 1
  fi
else
  git checkout -B "${BRANCH}" "${BASE}"
  echo "==> cherry-picking ${LAST}"
  if ! git cherry-pick "${LAST}"; then
    echo "" >&2
    echo "!! conflicts — resolve, then finish with:" >&2
    echo "   cd \"${REPO}\"" >&2
    echo "   git status" >&2
    echo "   # edit the conflicted files" >&2
    echo "   git add -A && git cherry-pick --continue" >&2
    echo "   \"${HERE}/$(basename "$0")\" ${NEW}" >&2
    echo "" >&2
    echo "conflicted files:" >&2
    git diff --name-only --diff-filter=U >&2
    exit 1
  fi
fi

git tag -a -f "${NEWTAG}" -m "OCMC ${NEW}: upstream ${NEW} plus multi-config menu" >/dev/null
git diff "${BASE}".."${NEWTAG}" > "${HERE}/../patches/ocmc-${NEW}.patch"

echo "==> done"
echo "    tag     ${NEWTAG}"
echo "    patch   patches/ocmc-${NEW}.patch ($(wc -l < "${HERE}/../patches/ocmc-${NEW}.patch" | tr -d ' ') lines)"
echo ""
echo "next: materialise a build tree and compile"
echo "    mkdir -p \"${REPO}/../OpenCorePkg-${NEW}\""
echo "    git -C \"${REPO}\" archive \"${NEWTAG}\" | tar -x -C \"${REPO}/../OpenCorePkg-${NEW}\""
echo "    cp -R \"${REPO}/../OpenCorePkg-1.0.8/UDK\" \"${REPO}/../OpenCorePkg-${NEW}/\"   # reuse EDK2"
echo "    # then run build-ocmc.sh against the new tree"
