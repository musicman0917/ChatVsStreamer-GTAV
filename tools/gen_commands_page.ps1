#Requires -Version 5.1
<#
  Writes docs/catalog.js, the data behind the commands page (docs/index.html), from the mod's
  own source: every effect in the table in src/Effects.cpp, the default settings in
  src/Settings.cpp, and the point rules in src/Economy.cpp and src/Mod.cpp.

    powershell -NoProfile -ExecutionPolicy Bypass -File tools\gen_commands_page.ps1

  build.ps1 runs this every time, so the page can't fall behind the mod. Anything it can't
  read stops it with the reason, rather than leaving an effect or a number off the page.
#>
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$Out = (Join-Path (Split-Path -Parent $PSScriptRoot) "docs\catalog.js")
)

$ErrorActionPreference = "Stop"
$invariant = [Globalization.CultureInfo]::InvariantCulture

function Read-Source($relative) { return [IO.File]::ReadAllText((Join-Path $Root $relative)) }

# The part of a file from the first line matching $start to the first line after it matching $end.
function Get-Block($text, $start, $end, $what) {
    $lines = $text -split "`r?`n"
    $from = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($from -lt 0 -and $lines[$i] -match $start) { $from = $i; continue }
        if ($from -ge 0 -and $lines[$i] -match $end) { return $lines[$from..$i] }
    }
    throw "Couldn't find $what. Update tools\gen_commands_page.ps1 to match the source."
}

function Format-Json($value) {
    if ($null -eq $value) { return "null" }
    if ($value -is [bool]) { if ($value) { return "true" } else { return "false" } }
    if ($value -is [int] -or $value -is [long]) { return $value.ToString($invariant) }
    if ($value -is [double]) { return $value.ToString("R", $invariant) }
    if ($value -is [string]) {
        $sb = New-Object Text.StringBuilder
        [void]$sb.Append('"')
        foreach ($ch in $value.ToCharArray()) {
            switch ($ch) {
                '"' { [void]$sb.Append('\"') }
                '\' { [void]$sb.Append('\\') }
                default {
                    if ([int]$ch -lt 0x20) { [void]$sb.Append(('\u{0:x4}' -f [int]$ch)) }
                    else { [void]$sb.Append($ch) }
                }
            }
        }
        [void]$sb.Append('"')
        return $sb.ToString()
    }
    if ($value -is [Collections.IDictionary]) {
        $parts = foreach ($key in $value.Keys) { (Format-Json ([string]$key)) + ":" + (Format-Json $value[$key]) }
        return "{" + ($parts -join ",") + "}"
    }
    if ($value -is [Collections.IEnumerable]) {
        $parts = foreach ($item in $value) { Format-Json $item }
        return "[" + ($parts -join ",") + "]"
    }
    throw "Can't write a $($value.GetType().Name) as JSON."
}

# ── Effects ──────────────────────────────────────────────────────────────────

$table = Get-Block (Read-Source "src\Effects.cpp") 'const std::vector<EffectDef>& AllEffects\(\)' 'return effects;' "the effect table in src\Effects.cpp"
$entry = '^\s*\{\s*"(?<id>[a-z0-9]+)",\s*"(?<name>[^"\\]+)",\s*(?<price>\d+),\s*Tier::(?<tier>Nuisance|Disruptive|Painful|Devastating|Blessing),\s*"(?<desc>[^"\\]+)",\s*(?<flags>[A-Za-z| ]+?),\s*(?<fn>[A-Za-z0-9_]+)(?:,\s*"(?<alias>[a-z0-9]+)")?\s*\},\s*$'
# What each flag means on the page. DirectCommand becomes the shortcut instead.
$needs = [ordered]@{ NeedsVehicle = "vehicle"; NeedsOnFoot = "foot"; RiskyInMission = "mission"; NeedsHuman = "human"; Silent = "silent" }

$effects = New-Object Collections.Generic.List[object]
$seen = @{}
foreach ($line in $table) {
    if ($line -notmatch '^\s*\{\s*"') { continue }
    if ($line -notmatch $entry) { throw "Couldn't read this effect in src\Effects.cpp. Keep each one on one line, as the others are:`n$($line.Trim())" }
    $m = $Matches
    foreach ($word in @($m.id) + @(if ($m.alias) { $m.alias })) {
        if ($seen.ContainsKey($word)) { throw "'$word' is used twice in the effect table." }
        $seen[$word] = $true
    }
    $flags = @($m.flags -split '\|' | ForEach-Object { $_.Trim() } | Where-Object { $_ -and $_ -ne "NoFlags" })
    $known = @($needs.Keys) + "DirectCommand"
    foreach ($flag in $flags) { if ($known -notcontains $flag) { throw "The page doesn't know the flag $flag on '$($m.id)'. Add it to tools\gen_commands_page.ps1 and docs\index.html." } }
    $description = $m.desc
    # The page shows the alias as a command of its own, so the sentence saying so would repeat it.
    if ($m.alias) { $description = $description -replace ("\s*Also !" + [regex]::Escape($m.alias) + "\.$"), "" }
    $effects.Add([ordered]@{
        id = $m.id
        name = $m.name
        price = [int]$m.price
        tier = $m.tier
        description = $description
        direct = $flags -contains "DirectCommand"
        alias = if ($m.alias) { $m.alias } else { $null }
        needs = @($needs.Keys | Where-Object { $flags -contains $_ } | ForEach-Object { $needs[$_] })
    })
}
$entries = @($table | Where-Object { $_ -match '^\s*\{\s*"' }).Count
if ($effects.Count -eq 0 -or $effects.Count -ne $entries) { throw "Read $($effects.Count) of the $entries effects in src\Effects.cpp." }

# ── Settings and point rules ─────────────────────────────────────────────────

$wanted = "StartingPoints", "ChatBonus", "ChatCooldown", "PassivePoints", "PassiveMinutes", "SubBonus", "GiftBonus",
          "PointsPerBit", "RaidPerViewer", "RaidAttack", "RaidAttackMinViewers", "EffectCooldown", "MinSpacing",
          "QueueLimit", "BlockRiskyInMissions", "ChaosGods", "ChaosGodsQuietMinutes", "ChaosGodsPainful"
$defs = Get-Block (Read-Source "src\Settings.cpp") 'const std::vector<SettingDef>& SettingDefs\(\)' 'return defs;' "the settings table in src\Settings.cpp"
$settings = [ordered]@{}
foreach ($line in $defs) {
    if ($line -match '^\s*\{\s*"(?<key>\w+)",\s*(?<def>0x[0-9A-Fa-f]+|\d+),\s*"' -and $wanted -contains $Matches.key) {
        $settings[$Matches.key] = [int]$Matches.def
    }
}
foreach ($key in $wanted) { if (-not $settings.Contains($key)) { throw "Couldn't find the setting $key in src\Settings.cpp." } }

$multiplierBlock = Get-Block (Read-Source "src\Economy.cpp") 'double Economy::Multiplier\(int subTier\)' '^\s*}\s*$' "Economy::Multiplier in src\Economy.cpp"
$multipliers = [ordered]@{}
foreach ($line in $multiplierBlock) {
    if ($line -match 'case\s+(?<tier>\d+):\s*return\s+(?<x>\d+(?:\.\d+)?);') { $multipliers[$Matches.tier] = [double]::Parse($Matches.x, $invariant) }
}
foreach ($tier in "1", "2", "3") { if (-not $multipliers.Contains($tier)) { throw "Couldn't read the tier $tier multiplier in src\Economy.cpp." } }

$economy = Read-Source "src\Economy.cpp"
if ($economy -notmatch 'double window = m_settings\.Get\("PassiveMinutes"\) \* 60\.0 \* (?<n>\d+);') { throw "Couldn't read how recently a viewer must have chatted for passive points, in src\Economy.cpp." }
$passiveWindow = [int]$Matches.n

$mod = Read-Source "src\Mod.cpp"
if ($mod -notmatch 'kQueueTimeoutSec = (?<s>\d+)(?:\.0)?;') { throw "Couldn't read kQueueTimeoutSec in src\Mod.cpp." }
$queueTimeout = [int]$Matches.s
if ($mod -notmatch 'p\.count = std::max\(1, ev\.count / (?<n>\d+)\);') { throw "Couldn't read how many raiders make one attacker, in src\Mod.cpp." }
$perAttacker = [int]$Matches.n
if ((Read-Source "src\Effects.cpp") -notmatch 'EffectResult AngryMob\(EffectContext& c\)\s*\{\s*int size = c\.count > 0 \? std::clamp\(c\.count, (?<min>\d+), (?<max>\d+)\)') { throw "Couldn't read the raid mob's size limits in AngryMob, in src\Effects.cpp." }
$mobMin = [int]$Matches.min
$mobMax = [int]$Matches.max

$rewardBlock = Get-Block $mod 'constexpr const char\* kRewardsTemplate =' ';\s*$' "kRewardsTemplate in src\Mod.cpp"
$rewards = New-Object Collections.Generic.List[object]
foreach ($line in $rewardBlock) {
    if ($line -match '^\s*"(?<title>[^"#=]+?)\s*=\s*(?<points>\d+)\\n"') { $rewards.Add([ordered]@{ title = $Matches.title; points = [int]$Matches.points }) }
}
if ($rewards.Count -eq 0) { throw "Couldn't read the default channel point rewards in src\Mod.cpp." }

# ── Write ────────────────────────────────────────────────────────────────────

# One effect per line, so a change to the table is a one-line change here.
$text = New-Object Text.StringBuilder
[void]$text.Append("// GENERATED by tools/gen_commands_page.ps1 from the mod's source. Do not edit: build.ps1 rewrites it.`n")
[void]$text.Append("window.CVS_CATALOG = {`n")
[void]$text.Append("  `"effects`": [`n")
for ($i = 0; $i -lt $effects.Count; $i++) {
    [void]$text.Append("    " + (Format-Json $effects[$i]) + $(if ($i -lt $effects.Count - 1) { "," } else { "" }) + "`n")
}
[void]$text.Append("  ],`n")
[void]$text.Append("  `"settings`": " + (Format-Json $settings) + ",`n")
[void]$text.Append("  `"subMultipliers`": " + (Format-Json $multipliers) + ",`n")
[void]$text.Append("  `"passiveWindowIntervals`": " + (Format-Json $passiveWindow) + ",`n")
[void]$text.Append("  `"queueTimeoutSeconds`": " + (Format-Json $queueTimeout) + ",`n")
[void]$text.Append("  `"raidMob`": " + (Format-Json ([ordered]@{ raidersPerAttacker = $perAttacker; min = $mobMin; max = $mobMax })) + ",`n")
[void]$text.Append("  `"rewards`": " + (Format-Json $rewards) + "`n")
[void]$text.Append("};`n")

$new = $text.ToString()
$old = if (Test-Path -LiteralPath $Out) { [IO.File]::ReadAllText($Out) } else { $null }
if ($new -ne $old) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Out) | Out-Null
    [IO.File]::WriteAllText($Out, $new, (New-Object Text.UTF8Encoding $false))
    Write-Host "Wrote $Out ($($effects.Count) effects)"
} else {
    Write-Host "$Out is up to date ($($effects.Count) effects)"
}
