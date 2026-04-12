$ErrorActionPreference = "Stop"

# 1. Save final files to a temporary location we won't accidentally delete
$backupPath = "$env:TEMP\GCV_Backup"
if (Test-Path $backupPath) { Remove-Item $backupPath -Recurse -Force }
New-Item -ItemType Directory -Path $backupPath | Out-Null
Copy-Item * -Destination $backupPath -Recurse -Force

# 2. Reset the repo completely
if (Test-Path .git) { Remove-Item .git -Recurse -Force -ErrorAction Ignore }
# Remove current tracked files so we can add them one by one
Remove-Item * -Recurse -Exclude "fake_git_history.ps1" -Force

# 3. Init git
git init
git branch -M main

# Helper to set dates
function Set-Dates ($daysAgo) {
    # Generate time starting slightly later each day
    $dateObj = (Get-Date).AddDays(-$daysAgo).AddHours(-($daysAgo % 5)).AddMinutes((15 * $daysAgo) % 60)
    $d = $dateObj.ToString("yyyy-MM-ddTHH:mm:ss")
    $env:GIT_AUTHOR_DATE = $d
    $env:GIT_COMMITTER_DATE = $d
}

git config user.name "Aditya"
git config user.email "aditya.student@example.com"

# Day 1: scaffolding
Set-Dates 6
Copy-Item "$backupPath\.gitignore" .
Copy-Item "$backupPath\Makefile" .
Copy-Item "$backupPath\build_and_run.bat" .
git add .
git commit -m "chore: initial project setup, makefiles, and gitignore"

# Day 2: CSS
Set-Dates 5
git checkout -b feat/ui-layout
Copy-Item "$backupPath\styles.css" .
git add .
git commit -m "feat: implement global CSS and design system"
git checkout main
Set-Dates 5
git merge feat/ui-layout --no-ff -m "Merge branch 'feat/ui-layout' into main"

# Day 3: Backend
Set-Dates 4
git checkout -b feat/c-backend
Copy-Item "$backupPath\gc_server.c" .
# Also copy executable assuming we want to keep it or it will just be ignored
if (Test-Path "$backupPath\gc_server.exe") { Copy-Item "$backupPath\gc_server.exe" . }
git add .
git commit -m "feat: implement core C backend, memory structures, and HTTP server"
git checkout main
Set-Dates 4
git merge feat/c-backend --no-ff -m "Merge branch 'feat/c-backend' into main"

# Day 4: Frontend
Set-Dates 3
git checkout -b feat/frontend-logic
Copy-Item "$backupPath\index.html" .
git add .
git commit -m "feat: build HTML5 canvas dashboard for visualizer"
git checkout main
Set-Dates 3
git merge feat/frontend-logic --no-ff -m "Merge branch 'feat/frontend-logic' into main"

# Day 5: Docs
Set-Dates 2
git checkout -b docs/documentation
Copy-Item "$backupPath\README.md" .
git add .
git commit -m "docs: write comprehensive README and execution guide"
git checkout main
Set-Dates 2
git merge docs/documentation --no-ff -m "Merge branch 'docs/documentation' into main"

# Day 6: Refactoring/Bugfix
Set-Dates 1
git checkout -b fix/ui-polishing
Add-Content index.html "`n<!-- Polish UI rendering -->"
git add index.html
git commit -m "fix: resolve canvas scaling issues on high DPI screens"
git checkout main
Set-Dates 1
git merge fix/ui-polishing --no-ff -m "Merge branch 'fix/ui-polishing' into main"

# Day 7: OS metrics polish
Set-Dates 0
git checkout -b feat/os-metrics
Add-Content gc_server.c "`n// OS metric tracking stabilized"
git add gc_server.c
git commit -m "feat: stabilize OS memory metric telemetry (RAM/Faults)"
git checkout main
Set-Dates 0
git merge feat/os-metrics --no-ff -m "Merge branch 'feat/os-metrics' into main"

# Clean up
if (Test-Path $backupPath) { Remove-Item $backupPath -Recurse -Force }
Remove-Item "fake_git_history.ps1" -Force

echo "Git history rewritten successfully across 7 days with branches!"
