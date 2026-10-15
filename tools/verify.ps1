<#
.SYNOPSIS
  Verifies the repository: the rails, the two checked copies, the host tests, the firmware build
  and the partition table, all inside the pinned ESP-IDF image. Green here is the bar for pushing
  (S-10).

.DESCRIPTION
  First, on this machine and in no container: tools/sync-agent-core.ps1 -Verify, which holds the
  copied agent core and its test support against the MANIFEST.txt of each tree (task p4-01). It is
  here rather than in verify.sh because the copier is a PowerShell script and because a copy that
  is not what it says it is makes everything after it meaningless; it needs no network and no
  sibling clone.

  Then: reads tools/idf-image.txt (tag and digest), pulls the image by digest if it is not present,
  and runs tools/verify.sh in it with the repository bind-mounted at /project, a named volume for
  the build directory (/build) and one for the caches (/ccache). Logs land in out/verify/, the
  images in out/images/. Runs from Windows PowerShell 5.1, and from pwsh on Linux - CI runs exactly
  this.

  The host-tests step begins with tools/check_rails.py: every rail this firmware promises, one
  entry each, the greps among them (task p3-06). `python tools/check_rails.py --list` prints the
  table without running anything.

  .\tools\verify.ps1                 everything
  .\tools\verify.ps1 -Only build     one step: spec, spec-tools, deps, host-tests, release-tests,
                                     build, partitions, release
  .\tools\verify.ps1 -Clean          remove the build and cache volumes first (a cold run)
  .\tools\verify.ps1 -Shell          a bash in the same container, same mounts, for a look around
  .\tools\verify.ps1 -Version 0.3.1+1a2b3c4
                                     the version baked into the build and carried by the dry run's
                                     manifest and by the descriptor inside the image; the default
                                     is 0.0.0-dev. The release workflow passes the release's.
  .\tools\verify.ps1 -TestKey        the test-key build (task p2-01): the specification's public
                                     test key in the trust table, the version stamped +TESTKEY, a
                                     warning at every boot; the release step expects the tooling
                                     to refuse it. Never flash this to a unit in service, and run
                                     the script once more without it afterwards.
#>
[CmdletBinding()]
param(
    [ValidateSet('', 'spec', 'spec-tools', 'deps', 'host-tests', 'release-tests', 'build', 'partitions', 'release')]
    [string]$Only = '',
    [switch]$Clean,
    [switch]$Shell,
    [string]$Version = '',
    [switch]$TestKey
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# The checked copies of the agent core (task p4-01, docs/upstream.md), before anything is built:
# components/agent-core/ is the library the board build compiles and host_test/agent-core/ is the
# fakes, the fixtures and the library's own two suites, each byte for byte what its MANIFEST.txt
# says and both at one commit. -Shell is a look around and skips it.
if (-not $Shell) {
    & (Join-Path $PSScriptRoot 'sync-agent-core.ps1') -Verify
    if ($LASTEXITCODE -ne 0) { throw 'the agent-core copy is not what its manifests say; nothing was built' }
}

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
# The version, only when one was asked for: verify.sh's default (0.0.0-dev) is then the one place
# it is written down.
if ($Version) { $mounts += @('-e', "TWINOAK_VERSION=$Version") }
# The test-key build, only when asked for; verify.sh passes 0 or 1 to cmake every time.
if ($TestKey) { $mounts += @('-e', 'TWINOAK_TRUST_TEST_KEY=1') }

if ($Shell) {
    & docker run --rm -it @mounts $image bash
    exit $LASTEXITCODE
}

$started = Get-Date
Write-Host "verify: $image"
if ($TestKey) { Write-Host "        A TEST-KEY BUILD (+TESTKEY): trusts the specification's public test key; never released" }
Write-Host "        repo $root"
Write-Host "        volumes $buildVolume (build), $cacheVolume (caches)"
# The image's entrypoint sources ESP-IDF's export.sh, then runs the command. No TTY: CI has none.
$cmd = 'bash tools/verify.sh'
if ($Only) { $cmd += " $Only" }
& docker run --rm @mounts $image bash -c $cmd
$code = $LASTEXITCODE
$elapsed = [int]((Get-Date) - $started).TotalSeconds
if ($code -eq 0) {
    Write-Host "verify: green in $elapsed s (logs in out/verify/, images in out/images/, the dry run's release in out/release/)"
} else {
    Write-Host "verify: FAILED (exit $code) after $elapsed s - see out/verify/*.log"
}
exit $code
