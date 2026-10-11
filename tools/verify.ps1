<#
.SYNOPSIS
  Verifies the repository: the spec copy, the host tests, the firmware build and the partition
  table, all inside the pinned ESP-IDF image. Green here is the bar for pushing (S-10).

.DESCRIPTION
  Reads tools/idf-image.txt (tag and digest), pulls the image by digest if it is not present, and
  runs tools/verify.sh in it with the repository bind-mounted at /project, a named volume for the
  build directory (/build) and one for the caches (/ccache). Logs land in out/verify/, the images in
  out/images/. Runs from Windows PowerShell 5.1, and from pwsh on Linux - CI runs exactly this.

  .\tools\verify.ps1                 everything
  .\tools\verify.ps1 -Only build     one step: spec, spec-tools, host-tests, build, partitions
  .\tools\verify.ps1 -Clean          remove the build and cache volumes first (a cold run)
  .\tools\verify.ps1 -Shell          a bash in the same container, same mounts, for a look around
#>
[CmdletBinding()]
param(
    [ValidateSet('', 'spec', 'spec-tools', 'host-tests', 'build', 'partitions')]
    [string]$Only = '',
    [switch]$Clean,
    [switch]$Shell
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# The image, from the one place that names it.
$pin = @{}
Get-Content (Join-Path $PSScriptRoot 'idf-image.txt') | ForEach-Object {
    if ($_ -match '^\s*(tag|digest)\s*=\s*(\S+)') { $pin[$matches[1]] = $matches[2] }
}
if (-not $pin.tag -or -not $pin.digest) { throw 'tools/idf-image.txt must give tag= and digest=' }
$image = "espressif/idf:$($pin.tag)@$($pin.digest)"

# Docker must be running.
& docker version --format '{{.Server.Version}}' 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'docker is not running (Docker Desktop on Windows); nothing can be verified without it' }

# One build volume and one cache volume per clone, keyed by the clone's path so two checkouts do not
# share a build directory.
$sha = [System.Security.Cryptography.SHA256]::Create()
$key = ([System.BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($root.ToLowerInvariant()))) -replace '-', '').Substring(0, 12).ToLowerInvariant()
$buildVolume = "twinoak-walter-build-$key"
$cacheVolume = "twinoak-walter-cache-$key"

if ($Clean) {
    foreach ($v in @($buildVolume, $cacheVolume)) {
        & docker volume rm -f $v 2>$null | Out-Null
    }
    Write-Host "removed volumes $buildVolume and $cacheVolume (a cold run)"
}

# Pull by digest when the image is not already here (the digest is the manifest list's, so docker
# resolves it on amd64 and arm64 alike).
& docker image inspect $image 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "pulling $image"
    & docker pull $image
    if ($LASTEXITCODE -ne 0) { throw "could not pull $image" }
}

$mounts = @(
    '-v', "${root}:/project",
    '-v', "${buildVolume}:/build",
    '-v', "${cacheVolume}:/ccache",
    '-w', '/project',
    '-e', 'TWINOAK_BUILD_DIR=/build',
    '-e', 'TWINOAK_CACHE_DIR=/ccache'
)

if ($Shell) {
    & docker run --rm -it @mounts $image bash
    exit $LASTEXITCODE
}

$started = Get-Date
Write-Host "verify: $image"
Write-Host "        repo $root"
Write-Host "        volumes $buildVolume (build), $cacheVolume (caches)"
# The image's entrypoint sources ESP-IDF's export.sh, then runs the command. No TTY: CI has none.
$cmd = 'bash tools/verify.sh'
if ($Only) { $cmd += " $Only" }
& docker run --rm @mounts $image bash -c $cmd
$code = $LASTEXITCODE
$elapsed = [int]((Get-Date) - $started).TotalSeconds
if ($code -eq 0) {
    Write-Host "verify: green in $elapsed s (logs in out/verify/, images in out/images/)"
} else {
    Write-Host "verify: FAILED (exit $code) after $elapsed s - see out/verify/*.log"
}
exit $code
