# Fetches SingleStepTests/m68000 (MIT), the CPU gate for the 68008 core, into
# .test-suites/m68000 at the root of the repository. That folder is gitignored:
# the suite is 317,500 tests in 127 files, about 130 MB, and is never committed.
#
#   .\scripts\fetch-m68000-tests.ps1
#
# The commit is pinned rather than following main, because the suite's README
# says its format may still change, and a test run should mean the same thing
# from one day to the next. Moving the pin is a deliberate change, made here.
#
# Only v1/*.json.bin is used. The core's runner reads that binary form directly
# (see core/tests/m68000_file.h), so decode.py is never run.
param(
    [string]$Destination
)

$ErrorActionPreference = 'Stop'

$repository = 'https://github.com/SingleStepTests/m68000.git'
$commit = '64b253116a3de04aaac4346c43680960dc9b67e5'   # main, 2024-08-01

$root = Split-Path -Parent $PSScriptRoot
if (-not $Destination) { $Destination = Join-Path $root '.test-suites\m68000' }

if (-not (Test-Path (Join-Path $Destination '.git'))) {
    New-Item -ItemType Directory -Force $Destination | Out-Null
    git -C $Destination init --quiet
    if ($LASTEXITCODE -ne 0) { throw "git init failed in $Destination" }
    git -C $Destination remote add origin $repository
    if ($LASTEXITCODE -ne 0) { throw "git remote add failed in $Destination" }
}

# A shallow fetch of the one commit: the history is of no use to the runner,
# and fetching it would multiply the download.
git -C $Destination fetch --quiet --depth 1 origin $commit
if ($LASTEXITCODE -ne 0) { throw "Fetching $commit from $repository failed" }
git -C $Destination checkout --quiet --force FETCH_HEAD
if ($LASTEXITCODE -ne 0) { throw "Checking out $commit failed" }

$files = @(Get-ChildItem (Join-Path $Destination 'v1') -Filter '*.json.bin')
Write-Host "OK: $($files.Count) test files at $commit in $Destination"
