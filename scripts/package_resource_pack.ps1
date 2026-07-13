param(
    [Parameter(Mandatory = $true)]
    [string] $SourceDir,

    [Parameter(Mandatory = $true)]
    [string] $DestinationPath
)

$ErrorActionPreference = "Stop"

$source = Resolve-Path -LiteralPath $SourceDir
$destination = [System.IO.Path]::GetFullPath($DestinationPath)
$destinationDir = [System.IO.Path]::GetDirectoryName($destination)

if (-not (Test-Path -LiteralPath (Join-Path $source "manifest.json"))) {
    throw "Resource pack manifest.json was not found in $source"
}

New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null

if (Test-Path -LiteralPath $destination) {
    Remove-Item -LiteralPath $destination -Force
}

$tempZip = [System.IO.Path]::ChangeExtension($destination, ".zip")
if (Test-Path -LiteralPath $tempZip) {
    Remove-Item -LiteralPath $tempZip -Force
}

Compress-Archive -Path (Join-Path $source "*") -DestinationPath $tempZip -Force
Move-Item -LiteralPath $tempZip -Destination $destination -Force

Write-Host "Resource pack generated: $destination"
