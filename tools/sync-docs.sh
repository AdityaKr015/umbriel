#!/usr/bin/env bash
# Sync docs/*.md into ../noctalia-docs/src/content/docs/umbriel/ as .mdx.
# The design/ subdirectory is never matched by the top-level glob.
#
# Existing .mdx files keep their hand-written frontmatter (title, description);
# only the body is refreshed from the source .md. New files get a title derived
# from their first H1. A leading H1 matching the frontmatter title is dropped
# from the body, mirroring the convention in the docs site.
#
# index.mdx is hand-authored in the docs site (it imports Astro components),
# so a future docs/index.md is ignored rather than synced over it.
#
# Usage: scripts/sync-docs.sh [docs-site-root]
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
site_root="${1:-"$repo_root/../noctalia-docs"}"
dest_dir="$site_root/src/content/docs/umbriel"

mkdir -p "$dest_dir"

for md in "$repo_root"/docs/*.md; do
    [[ -e "$md" ]] || continue
    base="$(basename "$md" .md)"
    [[ "$base" == "index" ]] && continue
    mdx="$dest_dir/$base.mdx"

    h1="$(awk '/^# / { sub(/^# /, ""); print; exit }' "$md")"

    frontmatter=""
    if [[ -f "$mdx" ]]; then
        frontmatter="$(awk 'NR == 1 && $0 == "---" { fm = 1; print; next } fm && $0 == "---" { print; exit } fm { print }' "$mdx")"
    fi

    title=""
    if [[ -n "$frontmatter" ]]; then
        title="$(printf '%s\n' "$frontmatter" | awk '/^title: / { sub(/^title: /, ""); print; exit }')"
    fi
    if [[ -z "$title" ]]; then
        title="$h1"
        frontmatter="---
title: $title
---"
    fi
    if [[ -z "$title" ]]; then
        printf 'sync-docs: no title in %s, skipping\n' "$md" >&2
        continue
    fi

    {
        printf '%s\n' "$frontmatter"
        printf '\n'
        awk -v title="$title" '
            NR == 1 && $0 == "---" { fm = 1; next }
            fm == 1 && $0 == "---" { fm = 0; next }
            fm == 1 { next }
            seen == 0 && $0 == ("# " title) { seen = 1; dropped = 1; next }
            dropped == 1 && /^$/ { dropped = 0; seen = 1; next }
            { seen = 1; print }
        ' "$md"
    } > "$mdx.tmp"
    mv "$mdx.tmp" "$mdx"
    printf 'synced %s -> %s\n' "$base.md" "${mdx#"$site_root"/}"
done
