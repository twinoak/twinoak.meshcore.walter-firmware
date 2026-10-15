<#
.SYNOPSIS
  Is there an agent-core tag newer than the pin?

.DESCRIPTION
  Tier one of docs\upstream.md, and the only part of it worth having outside a workflow:
  .github\workflows\upstream.yml runs this weekly and opens a pull request when the answer is
  yes. Having it here means the answer can be asked by hand, and the workflow understood, without
  waiting a week or pushing a branch:

    .\tools\upstream-check.ps1                            # against the pin, over the network
    .\tools\upstream-check.ps1 -Upstream ..\twinoak.meshmaster.exitnode-firmware
                                                          # against the clone beside this one
    .\tools\upstream-check.ps1 -Version 0.1.4             # as if the pin were three tags back

  The tag family is `vX.Y.Z`, and a tag of that repository **is** the library's version: its
  library.json, its idf_component.yml and its agent/Version.h all carry the same number, and
  upstream's own test_library fails if one of them moves alone (docs/library.md §10). So the
  comparison is the version in the copy here against the newest tag there, and the commit the
  manifests pin against the commit that tag names - a tag can be moved and a commit cannot.

  It reads and writes nothing of this repository. Moving the pin is a re-copy
  (tools\sync-agent-core.ps1), on a branch, and never this script's.

  **The upstream repository is private**, so a check over the network needs a credential: a `gh
  auth` on this machine, or GH_TOKEN / GITHUB_TOKEN in the environment for the workflow's sake.
  With neither, point -Upstream at the clone beside this one, which is what a session here does.

.PARAMETER Version
  Pretend the copy is at this version (`0.1.4`, with or without the leading v). The "there is
  something newer" path without a real bump.

.PARAMETER Upstream
  Where to read the tags from: a git URL, or a path to a clone. Empty (the default) builds the
  GitHub URL from the repository the manifests name, so that upstream is named in one place.

.PARAMETER GitHubOutput
  Also append newer/tag/sha/previous_version/previous_sha to $env:GITHUB_OUTPUT, for the workflow.
#>
[CmdletBinding()]
param(
    [string]$Version = '',
    [string]$Upstream = '',
    [switch]$GitHubOutput
)

$ErrorActionPreference = 'Continue'   # native commands write to stderr; exit codes are checked by hand
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$manifest = Join-Path $root 'components\agent-core\MANIFEST.txt'
$versionHeader = Join-Path $root 'components\agent-core\include\agent\Version.h'

if (-not (Test-Path $manifest)) { throw "no $manifest - the agent core has not been copied" }
if (-not (Test-Path $versionHeader)) { throw "no $versionHeader" }

# The pin, and the repository it came from: both out of the manifest's header, which is the one
# place this repository names upstream (tools\sync-agent-core.ps1 writes it).
$pin = ''
$repo = ''
foreach ($line in [IO.File]::ReadAllLines($manifest)) {
    if ($line -match '^# source: (\S+?)/([^/ ]+)/.* at commit ([0-9a-f]{40})$') {
        $repo = "$($Matches[1])/$($Matches[2])"
        $pin = $Matches[3]
        break
    }
}
if (-not $pin) { throw "could not read the pinned commit from $manifest" }
if (-not $repo) { throw "could not read the upstream repository from $manifest" }

# The version the copy is at, as the library itself states it.
$current = $Version -replace '^v', ''
if (-not $current) {
    foreach ($line in [IO.File]::ReadAllLines($versionHeader)) {
        if ($line -match '^#define AGENT_CORE_VERSION "(\d+\.\d+\.\d+)"') { $current = $Matches[1]; break }
    }
}
if ($current -notmatch '^\d+\.\d+\.\d+$') { throw "could not read AGENT_CORE_VERSION from $versionHeader" }
$currentVersion = [version]$current

# The URL, built from the owner and name the manifest gave: upstream is named in one place.
if (-not $Upstream) { $Upstream = "https://github.com/$repo.git" }

# Every v* tag upstream has, newest by version and not by the order git prints them.
$refs = & git ls-remote --tags $Upstream 'refs/tags/v*' 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "could not read the tags of $Upstream"
    Write-Host ($refs -join "`n")
    Write-Host ''
    Write-Host "That repository is private. Over the network this needs a credential - a gh login"
    Write-Host "on this machine, or GH_TOKEN in the environment; by hand, point -Upstream at the"
    Write-Host "clone beside this one:"
    Write-Host "    .\tools\upstream-check.ps1 -Upstream ..\twinoak.meshmaster.exitnode-firmware"
    exit 2
}
$tags = @{}
foreach ($line in @($refs)) {
    # "<sha>\trefs/tags/v0.2.0" and, for an annotated tag, "...^{}" with the commit.
    if ($line -notmatch '^([0-9a-f]{40})\s+refs/tags/(v\d+\.\d+\.\d+)(\^\{\})?$') { continue }
    $sha = $Matches[1]; $name = $Matches[2]; $peeled = [bool]$Matches[3]
    # The peeled line is the commit and wins over the tag object: a pin has to be a commit.
    if ($peeled -or -not $tags.ContainsKey($name)) { $tags[$name] = $sha }
}
if ($tags.Count -eq 0) { throw "no v* tags at $Upstream" }

$newest = $tags.Keys | Sort-Object { [version]($_ -replace '^v', '') } | Select-Object -Last 1
$newestVersion = [version]($newest -replace '^v', '')
$newer = $newestVersion -gt $currentVersion

# What the commit list in a bump's pull request starts from. Normally the pin; under -Version it
# is that version's own tag, because a range from the real pin would not be the range this
# rehearsal is pretending to be in.
$previousSha = $pin
if ($Version -and $tags.ContainsKey("v$current")) { $previousSha = $tags["v$current"] }

Write-Host "upstream: $repo at $Upstream"
Write-Host "pinned:   v$current  $previousSha$(if ($Version) { "  (pretended; the manifests say $pin)" })"
Write-Host "newest:   $newest  $($tags[$newest])"
if ($newer) {
    Write-Host "newer:    yes - v$current -> $newest"
    Write-Host "          the bump is a re-copy: .\tools\sync-agent-core.ps1 -Ref $newest, then verify.ps1"
}
else {
    Write-Host "newer:    no - nothing to bump"
    if ($tags[$newest] -ne $pin -and -not $Version) {
        Write-Host "WARNING:  $newest names $($tags[$newest]) and the manifests pin $pin -"
        Write-Host "          the tag was moved after the copy was taken. Re-copy at the commit you mean."
    }
}

if ($GitHubOutput -and $env:GITHUB_OUTPUT) {
    @(
        "newer=$($newer.ToString().ToLowerInvariant())",
        "tag=$newest",
        "sha=$($tags[$newest])",
        "previous_version=v$current",
        "previous_sha=$previousSha"
    ) | Out-File -Append -Encoding utf8 $env:GITHUB_OUTPUT
}
