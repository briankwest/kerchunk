#!/bin/bash
# Generate debian/changelog from git history
#
# Usage: ./scripts/generate-changelog.sh <version>
#   e.g.: ./scripts/generate-changelog.sh 1.1.0

set -euo pipefail

# Fix git ownership check in CI/Docker containers
git config --global --add safe.directory "$(pwd)" 2>/dev/null || true

VERSION="${1:?Usage: $0 <version>}"
MAINTAINER="Brian West <brian@mcalester.net>"
DATE=$(date -R)
PACKAGE="kerchunk"

CHANGELOG="debian/changelog"

# Get commit messages since last tag (or all if no tags exist)
LAST_TAG=$(git describe --tags --abbrev=0 2>/dev/null || echo "")

if [ -n "$LAST_TAG" ]; then
    COMMITS=$(git log --pretty=format:"  * %s" "${LAST_TAG}..HEAD")
else
    COMMITS=$(git log --pretty=format:"  * %s")
fi

if [ -z "$COMMITS" ]; then
    COMMITS="  * No changes recorded."
fi

# Generate new changelog entry
NEW_ENTRY="${PACKAGE} (${VERSION}) stable; urgency=medium

${COMMITS}

 -- ${MAINTAINER}  ${DATE}"

# Prepend to existing changelog or create new one
if [ -f "$CHANGELOG" ]; then
    TMPFILE=$(mktemp)
    echo "$NEW_ENTRY" > "$TMPFILE"
    echo "" >> "$TMPFILE"
    cat "$CHANGELOG" >> "$TMPFILE"
    mv "$TMPFILE" "$CHANGELOG"
else
    echo "$NEW_ENTRY" > "$CHANGELOG"
fi

echo "Updated ${CHANGELOG} with version ${VERSION}"
