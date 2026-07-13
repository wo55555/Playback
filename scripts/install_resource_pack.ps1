param(
    [Parameter(Mandatory = $true)]
    [string] $SourceDir,

    [Parameter(Mandatory = $true)]
    [string] $ComMojangDir,

    [switch] $UpdateGlobalResources
)

$ErrorActionPreference = "Stop"

$source = (Resolve-Path -LiteralPath $SourceDir).Path
$manifestPath = Join-Path $source "manifest.json"
if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "Resource pack manifest.json was not found in $source"
}

$manifest = ConvertFrom-Json -InputObject (Get-Content -Raw -LiteralPath $manifestPath)
$packId = [string] $manifest.header.uuid
$version = @($manifest.header.version | ForEach-Object { [int] $_ })

$comMojang = [System.IO.Path]::GetFullPath($ComMojangDir)
$resourcePacksRoot = [System.IO.Path]::GetFullPath((Join-Path $comMojang "resource_packs"))
$destination = [System.IO.Path]::GetFullPath((Join-Path $resourcePacksRoot "playback-ui"))
$resourcePacksPrefix = $resourcePacksRoot.TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
if (-not $destination.StartsWith($resourcePacksPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
    $destination.Substring($resourcePacksPrefix.Length) -ne "playback-ui") {
    throw "Refusing to replace unexpected resource pack path: $destination"
}

if (Test-Path -LiteralPath $destination) {
    Remove-Item -LiteralPath $destination -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $destination | Out-Null
Copy-Item -Path (Join-Path $source "*") -Destination $destination -Recurse -Force

if ($UpdateGlobalResources) {
    $usersDir = [System.IO.Directory]::GetParent($comMojang).Parent.Parent.FullName
    foreach ($userDir in Get-ChildItem -LiteralPath $usersDir -Directory) {
        $globalPacksPath = Join-Path $userDir.FullName "games\com.mojang\minecraftpe\global_resource_packs.json"
        if (-not (Test-Path -LiteralPath $globalPacksPath)) {
            continue
        }

        $parsedEntries = ConvertFrom-Json -InputObject (Get-Content -Raw -LiteralPath $globalPacksPath)
        if ($null -ne $parsedEntries.value -and $null -ne $parsedEntries.Count) {
            $parsedEntries = $parsedEntries.value
        }

        $entries = New-Object "System.Collections.Generic.List[object]"
        foreach ($parsedEntry in $parsedEntries) {
            $entries.Add($parsedEntry)
        }

        $matchingCount = 0
        foreach ($entry in $entries) {
            if ($entry.pack_id -ne $packId) {
                continue
            }
            $entry | Add-Member -NotePropertyName "version" -NotePropertyValue $version -Force
            $matchingCount += 1
        }
        if ($matchingCount -eq 0) {
            $entries.Add([pscustomobject]@{ pack_id = $packId; version = $version })
        }

        $json = ConvertTo-Json -InputObject $entries.ToArray() -Depth 5
        [System.IO.File]::WriteAllText(
            $globalPacksPath,
            $json + [Environment]::NewLine,
            [System.Text.UTF8Encoding]::new($false)
        )
    }
}

Write-Host "Resource pack installed: $destination"
