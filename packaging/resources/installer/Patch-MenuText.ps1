<#
    GoldenEye 007 PC - menu text patch

    Rewrites a few Xbox LIVE Arcade strings in the installed game data
    (Game\assets\files\loc\english\title\default.str) so the menus read right
    on PC:

      ACHIEVEMENTS             -> PC SETTINGS    (the item opens the PC menu)
      View your achievements   -> PC graphics & controls
      RETURN TO ARCADE         -> EXIT GAME
      System Link              -> LAN or Virtual LAN
      CREATE SYSTEM LINK GAME: -> CREATE LAN GAME:
      JOIN SYSTEM LINK GAME:   -> JOIN LAN OR VIRTUAL LAN GAME:

    The file holds an LSB2 string bank: a table of (u16 id, u32 offset in
    UTF-16 characters) entries followed by one block of NUL-terminated
    UTF-16BE strings. A string may be rewritten in place when it fits in its
    own slot. A longer one takes the slots of neighbours this port never shows
    (or rewrites both), and the table entry of the moved string is repointed.
    Every original string is checked before anything is written, so a file
    from another build is left alone, and a patched file is recognised and
    skipped.

      -AssetsDir <dir>   the folder holding default.xex and files\
    Exit code 0 when the file is patched or already was, 1 when it could not be.
#>
[CmdletBinding()]
param([Parameter(Mandatory = $true)] [string]$AssetsDir)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$path = Join-Path $AssetsDir 'files\loc\english\title\default.str'
if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    Write-Output "Menu text: $path not found."
    exit 1
}
$bytes = [System.IO.File]::ReadAllBytes($path)

# Layout of this build's bank (checked below, never assumed blindly).
$blobStart = 3198

function Get-Utf16([string]$Text) {
    return [System.Text.Encoding]::BigEndianUnicode.GetBytes($Text + [char]0)
}

function Test-TextAt([int]$Offset, [string]$Text) {
    $want = Get-Utf16 $Text
    if ($Offset + $want.Length -gt $bytes.Length) { return $false }
    for ($i = 0; $i -lt $want.Length; $i++) {
        if ($bytes[$Offset + $i] -ne $want[$i]) { return $false }
    }
    return $true
}

function Read-EntryOffset([int]$EntryAt) {
    return ([int]$bytes[$EntryAt + 2] -shl 24) -bor ([int]$bytes[$EntryAt + 3] -shl 16) -bor
        ([int]$bytes[$EntryAt + 4] -shl 8) -bor [int]$bytes[$EntryAt + 5]
}

function Write-EntryOffset([int]$EntryAt, [int]$CharOffset) {
    $bytes[$EntryAt + 2] = [byte](($CharOffset -shr 24) -band 0xFF)
    $bytes[$EntryAt + 3] = [byte](($CharOffset -shr 16) -band 0xFF)
    $bytes[$EntryAt + 4] = [byte](($CharOffset -shr 8) -band 0xFF)
    $bytes[$EntryAt + 5] = [byte]($CharOffset -band 0xFF)
}

# One region = consecutive original strings (with their table entries) that
# are replaced by consecutive new strings. Each new string names the table
# entry that should point at it.
$regions = @(
    @{ Start = 10618
        Old = @(@{ Entry = 2274; Text = "ACHIEVEMENTS`n" })
        New = @(@{ Entry = 2274; Text = "PC SETTINGS`n" }) },
    @{ Start = 10714
        Old = @(@{ Entry = 2292; Text = "RETURN TO ARCADE`n" })
        New = @(@{ Entry = 2292; Text = "EXIT GAME`n" }) },
    @{ Start = 19510
        Old = @(@{ Entry = 3096; Text = 'View your achievements' })
        New = @(@{ Entry = 3096; Text = 'PC graphics & controls' }) },
    @{ Start = 10860
        Old = @(@{ Entry = 2328; Text = 'CREATE SYSTEM LINK GAME:' }, @{ Entry = 2334; Text = 'JOIN SYSTEM LINK GAME:' })
        New = @(@{ Entry = 2328; Text = 'CREATE LAN GAME:' }, @{ Entry = 2334; Text = 'JOIN LAN OR VIRTUAL LAN GAME:' }) },
    @{ Start = 12628
        Old = @(@{ Entry = 2688; Text = 'System Link' }, @{ Entry = 2694; Text = 'Xbox Live' })
        New = @(@{ Entry = 2688; Text = 'LAN or Virtual LAN' }, @{ Entry = 2694; Text = '' }) }
)

# Where each new string lands, and whether the old or new text is present.
$allOld = $true
$allNew = $true
foreach ($region in $regions) {
    $size = 0
    $at = $region.Start
    foreach ($old in $region.Old) {
        if (-not (Test-TextAt $at $old.Text)) { $allOld = $false }
        if ((Read-EntryOffset $old.Entry) -ne ($at - $blobStart) / 2) { $allOld = $false }
        $at += (Get-Utf16 $old.Text).Length
    }
    $region.Size = $at - $region.Start
    $at = $region.Start
    foreach ($new in $region.New) {
        $new.At = $at
        if (-not (Test-TextAt $at $new.Text)) { $allNew = $false }
        if ((Read-EntryOffset $new.Entry) -ne ($at - $blobStart) / 2) { $allNew = $false }
        $at += (Get-Utf16 $new.Text).Length
    }
    if ($at - $region.Start -gt $region.Size) {
        throw ('Menu text does not fit at {0}.' -f $region.Start)
    }
}

if ($allNew) {
    Write-Output 'Menu text: already patched.'
    exit 0
}
if (-not $allOld) {
    Write-Output 'Menu text: this default.str is not the expected build; left unchanged.'
    exit 1
}

foreach ($region in $regions) {
    for ($i = 0; $i -lt $region.Size; $i++) { $bytes[$region.Start + $i] = 0 }
    foreach ($new in $region.New) {
        $data = Get-Utf16 $new.Text
        [Array]::Copy($data, 0, $bytes, $new.At, $data.Length)
        Write-EntryOffset $new.Entry (($new.At - $blobStart) / 2)
    }
}
[System.IO.File]::WriteAllBytes($path, $bytes)
Write-Output ('Menu text: patched {0} strings.' -f ($regions | ForEach-Object { $_.New.Count } | Measure-Object -Sum).Sum)
exit 0
