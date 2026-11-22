#!/usr/bin/env bash
# Prepare and commit a local merge candidate. The workflow builds it before publishing.
set -euo pipefail

upstream_url=${UPSTREAM_URL:-https://github.com/8BiTw0LF/VeyAmux.git}
upstream_branch=${UPSTREAM_BRANCH:-main}
summary() {
    printf '%s\n' "$*"
    if [[ -n ${GITHUB_STEP_SUMMARY:-} ]]; then printf '%s\n' "$*" >> "$GITHUB_STEP_SUMMARY"; fi
}
fail() { summary "$*" >&2; exit 1; }

result() {
    printf 'updated=%s\nbase=%s\ncommit=%s\ntag=%s\n' "$1" "$base" "$(git rev-parse HEAD)" "$2" >> "${GITHUB_OUTPUT:-/dev/stdout}"
}
[[ -z $(git status --porcelain) ]] || fail 'The fork checkout must be clean before syncing.'
git check-ref-format "refs/heads/$upstream_branch"
base=$(git rev-parse HEAD)
git fetch --no-tags "$upstream_url" "refs/heads/$upstream_branch"
incoming=$(git rev-parse 'FETCH_HEAD^{commit}')
if git merge-base --is-ancestor "$incoming" "$base"; then
    summary 'The fork already contains the upstream revision.'
    result false ''
    exit 0
fi
git merge-base "$base" "$incoming" >/dev/null || fail 'No common ancestor. Check out the full fork history before syncing.'
version=$(git show "$incoming:CMakeLists.txt" | sed -nE 's/^[[:blank:]]*project\([[:blank:]]*veyamux-linux[[:blank:]]+VERSION[[:blank:]]+([0-9]+(\.[0-9]+)+)([[:blank:]].*|\))$/\1/p')
[[ $version =~ ^[0-9]+(\.[0-9]+)+$ ]] || fail 'Could not determine the upstream VeyAmux version from CMakeLists.txt.'
tag="v${version}-nvidia-sync.${incoming:0:12}"
bot=(git -c 'user.name=github-actions[bot]' -c 'user.email=41898282+github-actions[bot]@users.noreply.github.com')
tmp=$(mktemp -d)
cleanup() {
    if git rev-parse -q --verify MERGE_HEAD >/dev/null; then git merge --abort; fi
    rm -rf -- "$tmp"
}
trap cleanup EXIT
if ! "${bot[@]}" merge --no-ff --no-commit "$incoming"; then
    git rev-parse -q --verify MERGE_HEAD >/dev/null || fail 'Git could not start a merge.'
fi

# The fork maintains its own short README. Upstream documentation remains available there by link.
git restore --source="$base" --staged --worktree -- README.md

# Upstream stamps a numeric version in main.cpp; the fork stamps the release at build time.
# Ignore just an upstream literal version bump, then let Git merge all other edits normally.
if [[ -n $(git ls-files -u -- src/main.cpp) ]]; then
    if git show :1:src/main.cpp > "$tmp/base" && git show :2:src/main.cpp > "$tmp/fork" && git show :3:src/main.cpp > "$tmp/upstream"; then
        literal='^[[:blank:]]*QCoreApplication::setApplicationVersion\("[0-9]+(\.[0-9]+)+"\);[[:blank:]]*$'
        if [[ $(grep -Ec "$literal" "$tmp/base" || true) == 1 && $(grep -Ec "$literal" "$tmp/upstream" || true) == 1 ]]; then
            old_line=$(grep -E "$literal" "$tmp/base")
            sed -E "s|$literal|$old_line|" "$tmp/upstream" > "$tmp/normalized"
            if git merge-file -p "$tmp/fork" "$tmp/base" "$tmp/normalized" > "$tmp/merged"; then
                cat "$tmp/merged" > src/main.cpp
                git add -- src/main.cpp
            fi
        fi
    fi
fi
conflicts=$(git diff --name-only --diff-filter=U)
[[ -z $conflicts ]] || fail "Manual merge required. Conflicting files:
$conflicts"

# A change to publication permissions or the fork's update channel needs human review.
protected=(.github/workflows .github/actions scripts/sync-upstream.sh scripts/build-appimage.sh
           src/AppUpdate.cpp src/AppUpdate.h src/PlayerBridgeUpdate.cpp packaging/veyamux.metainfo.xml)
changed=$(git diff --cached --name-only "$base" -- "${protected[@]}")
[[ -z $changed ]] || fail "Manual review required for CI, publishing or update-channel changes:
$changed"

"${bot[@]}" commit -m "Sync VeyAmux $version from upstream (${incoming:0:12})"
summary "Prepared $tag for build validation."
result true "$tag"
