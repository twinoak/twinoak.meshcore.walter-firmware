<#
.SYNOPSIS
  Brings agent-core and its test support across from the exit-node firmware at one commit, and
  checks that the copy is still what it says it is.

.DESCRIPTION
  The agent's job logic is twinoak/twinoak.meshmaster.exitnode-firmware's `lib/agent-core/`
  (docs/upstream.md, spec/proposals/03). It arrives here as a **checked copy at one commit** -
  the same arrangement `spec/` has, and for the same reason: the repository it comes from is
  private, and neither the build container on this machine nor the CI runner has a credential for
  it, so the component manager cannot clone it at build time (tasks/phase-4/reports/01.md says
  what would change that). The copy is never edited here. Two trees come across:

    components/agent-core/   is lib/agent-core/, with that prefix stripped: the library, as an
                             ESP-IDF component, its own CMakeLists.txt and idf_component.yml
                             included and unedited

    host_test/agent-core/    is upstream's own paths, unstripped:
                               test/support/              the four fakes, the fixture reader and
                                                          the job builders
                               test/test_runner/, test/test_loop/   the library's own two suites
                               tests/fixtures/contracts/  the agent API's envelopes
                               tests/fixtures/companion/  the transcribed MeshCore frames
                               platformio.ini             upstream's, for its [meshcore] pin

  The paths under host_test/agent-core/ are upstream's and not tidied, which is not neatness: the
  runner suite's `Citations` case reads `test/support/FakeMesh.h`, `platformio.ini`'s `[meshcore]`
  pin and `tests/fixtures/companion/` relative to a PROJECT_DIR it is handed, and laying the copy
  out upstream's way is what lets every case of both suites run here unmodified rather than with
  one filtered out. (The half of that case which reads the pinned MeshCore source is skipped -
  here and in upstream's own CI, because neither has a MeshCore checkout.)

  Each tree gets a MANIFEST.txt in the shape spec/MANIFEST.txt has - a SHA-256 and a size per
  file, and the commit in the header - and **the commit is in exactly those two places**, which
  host_test/rails/test_rail_agent_core.cpp holds equal. A MANIFEST.txt or a README.md at a tree's
  root is this repository's own line about the copy and is not in the manifest, as spec/'s README
  is not (spec/tools/specsync.py's LOCAL); a README.md *inside* the copy - the fixtures each have
  one - is upstream's and is.

    .\tools\sync-agent-core.ps1 -Ref v0.3.0      copy at that tag (or commit), write both manifests
    .\tools\sync-agent-core.ps1 -Verify          re-hash both trees against their manifests

  -Verify is run by tools/verify.ps1 before anything is built, and so by CI; it needs no network
  and no exit-node clone. Copying needs the clone, which is why a bump is a session on this
  machine and not a workflow (docs/upstream.md, tier two).

.PARAMETER Ref
  The tag or commit of the exit-node repository to copy. Required to copy; ignored by -Verify.

.PARAMETER Verify
  Check both trees against their manifests and the two commits against each other. Changes
  nothing.

.PARAMETER Repo
  The exit-node clone to read, which is read and never written (`git archive` of a commit).
  Default: ..\twinoak.meshmaster.exitnode-firmware beside this repository.

.PARAMETER Quiet
  One line per tree instead of a list of what was copied.
#>
[CmdletBinding()]
param(
    [string]$Ref = '',
    [switch]$Verify,
    [string]$Repo = '',
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# The upstream repository, named here and in docs/upstream.md and nowhere else.
$upstreamName = 'twinoak/twinoak.meshmaster.exitnode-firmware'

# The two trees. `Strip` is the prefix taken off each archived path as it lands; '' keeps
# upstream's own paths.
$trees = @(
    @{
        Dest  = 'components/agent-core'
        What  = 'The MeshMaster agent core, copied - never edited here.'
        From  = 'lib/agent-core'
        Strip = 'lib/agent-core'
        Paths = @('lib/agent-core')
    },
    @{
        Dest  = 'host_test/agent-core'
        What  = "The agent core's host fakes, contract fixtures and own two suites, copied - never edited here."
        From  = 'test/, tests/fixtures/ and platformio.ini'
        Strip = ''
        Paths = @('test/support', 'test/test_runner', 'test/test_loop',
            'tests/fixtures/contracts', 'tests/fixtures/companion', 'platformio.ini')
    }
)

$manifestName = 'MANIFEST.txt'
# What belongs to this repository and not to the copy, at a tree's root only.
$localFiles = @('MANIFEST.txt', 'README.md')
$manifestHeader = '# sha256                                                           bytes  name'

function Get-TreeFiles {
    # Every file of a copied tree, relative to the tree, sorted, with this repository's own two
    # root-level files left out.
    param([string]$TreeRoot)
    if (-not (Test-Path $TreeRoot)) { return @() }
    $prefix = (Resolve-Path $TreeRoot).Path
    Get-ChildItem -LiteralPath $prefix -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($prefix.Length + 1).Replace('\', '/')
        if ($localFiles -notcontains $rel) { $rel }
    } | Sort-Object
}

function Get-FileDigest {
    param([string]$Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = $sha.ComputeHash($bytes) } finally { $sha.Dispose() }
    [pscustomobject]@{
        Sha   = ([BitConverter]::ToString($hash) -replace '-', '').ToLowerInvariant()
        Bytes = $bytes.Length
    }
}

function Read-Manifest {
    # The commit out of the header and (name -> sha, bytes) out of the body. Throws when the file
    # is not a manifest this script wrote.
    param([string]$Path)
    if (-not (Test-Path $Path)) { throw "no $Path - run .\tools\sync-agent-core.ps1 -Ref <tag>" }
    $commit = ''
    $files = [ordered]@{}
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ($line -match '^# source: .+ at commit ([0-9a-f]{40})$') { $commit = $Matches[1]; continue }
        if ($line -match '^\s*#' -or $line.Trim() -eq '') { continue }
        if ($line -notmatch '^([0-9a-f]{64})\s+(\d+)\s+(.+)$') { throw "$Path has a line this script did not write: $line" }
        $files[$Matches[3]] = [pscustomobject]@{ Sha = $Matches[1]; Bytes = [int]$Matches[2] }
    }
    if (-not $commit) { throw "$Path does not say which commit it came from" }
    [pscustomobject]@{ Commit = $commit; Files = $files }
}

function Write-Manifest {
    param([string]$TreeRoot, [string]$What, [string]$From, [string]$Commit)
    $lines = New-Object Collections.Generic.List[string]
    $lines.Add("# $What")
    $lines.Add("# source: $upstreamName/$From at commit $Commit")
    $lines.Add('# written by tools/sync-agent-core.ps1; docs/upstream.md says how and when')
    $lines.Add('#')
    $lines.Add($manifestHeader)
    $n = 0
    foreach ($rel in Get-TreeFiles $TreeRoot) {
        $d = Get-FileDigest (Join-Path $TreeRoot $rel)
        $lines.Add(('{0}  {1,7}  {2}' -f $d.Sha, $d.Bytes, $rel))
        $n++
    }
    # LF and no byte-order mark: the same bytes on Windows and in the container, so that the
    # manifest of a tree does not itself depend on where the sync was run.
    [IO.File]::WriteAllText((Join-Path $TreeRoot $manifestName), (($lines -join "`n") + "`n"),
        (New-Object Text.UTF8Encoding($false)))
    return $n
}

function Set-TreeTimes {
    # One moment on every file and directory of a copied tree: the moment the sync ran. See the
    # call site for why a build's correctness depends on it.
    param([string]$TreeRoot, [datetime]$When)
    Get-ChildItem -LiteralPath $TreeRoot -Recurse -Force | ForEach-Object {
        $_.LastWriteTime = $When
    }
    (Get-Item -LiteralPath $TreeRoot).LastWriteTime = $When
}

function Test-Tree {
    param([string]$TreeRoot, [string]$Label)
    $manifest = Read-Manifest (Join-Path $TreeRoot $manifestName)
    $problems = New-Object Collections.Generic.List[string]
    foreach ($rel in $manifest.Files.Keys) {
        $path = Join-Path $TreeRoot $rel
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $problems.Add("missing: $Label/$rel")
            continue
        }
        $d = Get-FileDigest $path
        $want = $manifest.Files[$rel]
        if ($d.Bytes -ne $want.Bytes -or $d.Sha -ne $want.Sha) {
            $problems.Add(("changed: {0}/{1} ({2} bytes {3}, the manifest says {4} bytes {5})" -f
                    $Label, $rel, $d.Bytes, $d.Sha.Substring(0, 12), $want.Bytes, $want.Sha.Substring(0, 12)))
        }
    }
    foreach ($rel in (Get-TreeFiles $TreeRoot)) {
        if (-not $manifest.Files.Contains($rel)) { $problems.Add("not in the manifest: $Label/$rel") }
    }
    [pscustomobject]@{ Commit = $manifest.Commit; Count = $manifest.Files.Count; Problems = $problems }
}

# ---- -Verify ---------------------------------------------------------------------------------

if ($Verify) {
    $problems = New-Object Collections.Generic.List[string]
    $commits = [ordered]@{}
    foreach ($tree in $trees) {
        $label = $tree.Dest
        $result = Test-Tree (Join-Path $root $tree.Dest) $label
        $commits[$label] = $result.Commit
        foreach ($p in $result.Problems) { $problems.Add($p) }
        if (-not $Quiet) {
            Write-Host ("{0}: {1} files at {2}, {3}" -f $label, $result.Count, $result.Commit.Substring(0, 8),
                $(if ($result.Problems.Count -eq 0) { 'all as the manifest says' } else { "$($result.Problems.Count) PROBLEM(S)" }))
        }
    }
    # The pin is in two places on purpose - one per tree - and this is the half of holding them
    # equal that runs before anything is compiled; the other half is a host test, so that a
    # checkout which never runs this script is held to it too.
    $distinct = @($commits.Values | Sort-Object -Unique)
    if ($distinct.Count -ne 1) {
        $problems.Add('the two trees are at different commits: ' +
            (($commits.Keys | ForEach-Object { "$_ at $($commits[$_])" }) -join ', ') +
            ' - re-run the sync at one commit')
    }
    foreach ($p in $problems) { Write-Host "  $p" }
    if ($problems.Count -gt 0) {
        Write-Host 'agent-core: the copy is not what its manifest says. It is never edited here:'
        Write-Host '            restore it with .\tools\sync-agent-core.ps1 -Ref <the tag in the manifest>,'
        Write-Host '            or change it upstream and bump the pin (docs/upstream.md).'
        exit 1
    }
    Write-Host ("agent-core: the copy is at " + $distinct[0].Substring(0, 8) +
        " and is byte for byte what both manifests say")
    exit 0
}

# ---- the copy --------------------------------------------------------------------------------

if (-not $Ref) { throw 'give -Ref <tag or commit> to copy, or -Verify to check the copy' }

if (-not $Repo) { $Repo = Join-Path (Split-Path -Parent $root) 'twinoak.meshmaster.exitnode-firmware' }
if (-not (Test-Path (Join-Path $Repo '.git'))) {
    throw "no exit-node clone at $Repo. The copy is made on a machine that has one; -Verify needs none."
}

# The commit the ref names. A tag can be moved, so what goes into the manifests is always the
# commit (docs/library.md section 2: "pin the commit the tag names, not the tag").
$commit = (& git -C $Repo rev-parse "$Ref^{commit}" 2>$null)
if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-f]{40}') { throw "$Ref is not a ref of $Repo" }
$commit = $commit.Trim()
# The tag, if the commit is one, for the line below and nothing else. `git describe
# --exact-match` *fails* on a commit that is not tagged - which is every `-Ref <commit>` between
# two releases, the shape this script's own help documents - and PowerShell 5.1 turns a native
# command's stderr into an ErrorRecord, which under this script's `$ErrorActionPreference =
# 'Stop'` ended the sync before it copied anything (task p9-05: the first sync at a commit rather
# than a tag). `2>$null` does not help: the record is made before the redirection. So the
# preference is lowered for that one call and the exit code is what is read.
$describe = ''
$ErrorActionPreference = 'Continue'
$tag = & git -C $Repo describe --tags --exact-match $commit 2>$null
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -eq 0 -and $tag) { $describe = $tag.Trim() }
Write-Host "agent-core: copying $upstreamName at $commit$(if ($describe) { " ($describe)" })"
Write-Host "            from $Repo, which is read and never written"

# git archive of one commit into a tar, then tar -x: no checkout, no index, nothing of that
# repository's working tree touched (tasks/RUNBOOK.md: a sibling repository is read-only). A tar
# file rather than a pipe, because PowerShell 5.1 turns a native-to-native pipe into text and
# these trees have bytes in them that are not text.
$syncStarted = Get-Date
$work = Join-Path ([IO.Path]::GetTempPath()) ("agent-core-sync-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $tar = Join-Path $work 'upstream.tar'
    $paths = @()
    foreach ($tree in $trees) { $paths += $tree.Paths }
    & git -C $Repo archive --format=tar -o $tar $commit @paths
    if ($LASTEXITCODE -ne 0) { throw "git archive of $commit failed" }
    $extracted = Join-Path $work 'tree'
    New-Item -ItemType Directory -Path $extracted | Out-Null
    & tar -x -f $tar -C $extracted
    if ($LASTEXITCODE -ne 0) { throw 'tar could not extract the archive' }

    foreach ($tree in $trees) {
        $dest = Join-Path $root $tree.Dest
        # The copy replaces the tree, so that a file upstream deleted does not survive here. The
        # README.md this repository wrote for the tree is kept across the replacement; the
        # manifest is rewritten below anyway.
        $readme = Join-Path $dest 'README.md'
        $keptReadme = if (Test-Path -LiteralPath $readme -PathType Leaf) { [IO.File]::ReadAllBytes($readme) } else { $null }
        if (Test-Path $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
        New-Item -ItemType Directory -Path $dest -Force | Out-Null
        if ($null -ne $keptReadme) { [IO.File]::WriteAllBytes($readme, $keptReadme) }

        foreach ($path in $tree.Paths) {
            $source = Join-Path $extracted ($path -replace '/', '\')
            $landing = if ($tree.Strip -and $path -eq $tree.Strip) { '' }
            elseif ($tree.Strip) { $path.Substring($tree.Strip.Length).TrimStart('/') }
            else { $path }
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                $target = Join-Path $dest ($landing -replace '/', '\')
                New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
                Copy-Item -LiteralPath $source -Destination $target -Force
            }
            else {
                $target = if ($landing) { Join-Path $dest ($landing -replace '/', '\') } else { $dest }
                New-Item -ItemType Directory -Path $target -Force | Out-Null
                Copy-Item -Path (Join-Path $source '*') -Destination $target -Recurse -Force
            }
            if (-not $Quiet) { Write-Host ("            {0} -> {1}" -f $path, ($tree.Dest + '/' + $landing)) }
        }
        # Every copied file is stamped **now**, and this is not tidiness. `git archive` writes the
        # *commit's* timestamp into the tar and `tar -x` keeps it, so a copy made today of a commit
        # made yesterday lands with yesterday's mtime - older than the object files of a build that
        # ran in between. Ninja compares mtimes, so an incremental build then keeps a stale object
        # compiled against the *previous* header, and C++ answers that with a silent ODR
        # violation rather than an error: task p9-05's first build of this sync had `mm::Heartbeat`
        # at 152 bytes in a test and 168 in the library, and the suite died in the sanitizer
        # instead of saying what was wrong. CI never sees it (a fresh checkout stamps everything
        # at checkout), which is exactly why the local machine has to be right.
        Set-TreeTimes $dest $syncStarted
        $n = Write-Manifest $dest $tree.What $tree.From $commit
        Write-Host ("{0}: {1} files, MANIFEST.txt written" -f $tree.Dest, $n)
    }
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host 'agent-core: copied. Nothing of it is edited here; `-Verify` is what tools/verify.ps1 runs.'
