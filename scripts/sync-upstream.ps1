# Lilygo Crossmux Upstream Sync Utility
# Automates the process of fetching and merging updates from the upstream repository
# while preserving the structural integrity of the customization guide.

$ErrorActionPreference = "Stop"

$upstreamUrl = "https://github.com/crosspoint-reader/crosspoint-reader.git"
$remoteName = "upstream"

Write-Host "--- Lilygo Crossmux Sync Utility ---" -ForegroundColor Cyan

# 1. Ensure upstream remote exists
$remotes = git remote
if ($remotes -notcontains $remoteName) {
    Write-Host "Adding upstream remote..."
    git remote add $remoteName $upstreamUrl
}

# 2. Fetch latest changes
Write-Host "Fetching upstream..."
git fetch $remoteName

# 3. Check for structural guide conflicts (CLAUDE.md / .skills/SKILL.md)
# Per upstream-merge-policy.md: we keep our thin map and re-home technical details.
Write-Host "Merging upstream/main into main..."
try {
    git merge "$remoteName/main" --no-commit --no-ff
} catch {
    Write-Host "Merge conflicts detected. Handling known structural conflicts..." -ForegroundColor Yellow
}

# 4. Auto-resolve .skills/SKILL.md if it conflicts
$status = git status --short
if ($status -like "*UU .skills/SKILL.md*") {
    Write-Host "Resolving .skills/SKILL.md conflict using 'ours' (Thin Map strategy)..."
    git checkout --ours -- .skills/SKILL.md
    git add .skills/SKILL.md
}

Write-Host "`nMerge in progress. Please review remaining conflicts manually." -ForegroundColor Green
Write-Host "Once resolved, run: git commit"
Write-Host "Then verify with: pio run -e lilygo_h716"
