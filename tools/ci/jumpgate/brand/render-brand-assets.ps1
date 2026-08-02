# SPDX-License-Identifier: GPL-2.0-or-later

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$chrome = 'C:\Program Files\Google\Chrome\Application\chrome.exe'
$chromeProfile = Join-Path ([System.IO.Path]::GetTempPath()) `
  ('jumpgate-brand-' + [System.Guid]::NewGuid().ToString('N'))

if (-not (Test-Path -LiteralPath $chrome)) {
  throw "Chrome is required to render the committed brand assets: $chrome"
}

function Render-Svg {
  param(
    [Parameter(Mandatory)] [string] $Source,
    [Parameter(Mandatory)] [string] $Destination,
    [Parameter(Mandatory)] [int] $Width,
    [Parameter(Mandatory)] [int] $Height
  )

  $sourcePath = (Resolve-Path -LiteralPath $Source).Path
  $destinationPath = [System.IO.Path]::GetFullPath($Destination)
  $uri = [System.Uri]::new($sourcePath).AbsoluteUri
  New-Item -ItemType Directory -Force -Path ([System.IO.Path]::GetDirectoryName($destinationPath)) | Out-Null
  Remove-Item -LiteralPath $destinationPath -ErrorAction SilentlyContinue
  & $chrome --headless=new --disable-gpu --hide-scrollbars --allow-file-access-from-files `
    --no-first-run --user-data-dir="$chromeProfile" `
    --force-device-scale-factor=1 --default-background-color=00000000 `
    --virtual-time-budget=1000 --window-size="$Width,$Height" `
    --screenshot="$destinationPath" $uri 2>$null
  for ($attempt = 0; $attempt -lt 100 -and -not (Test-Path -LiteralPath $destinationPath); $attempt++) {
    Start-Sleep -Milliseconds 100
  }
  if (-not (Test-Path -LiteralPath $destinationPath)) {
    throw "Failed to render $Source"
  }
}

$mark = Join-Path $PSScriptRoot 'jumpgate-mark.svg'
$wordmark = Join-Path $PSScriptRoot 'jumpgate-wordmark.svg'
$splash = Join-Path $PSScriptRoot 'jumpgate-splash.svg'
$banner = Join-Path $PSScriptRoot 'jumpgate-banner.svg'
$notification = Join-Path $PSScriptRoot 'jumpgate-notification.svg'
$webLogo = Join-Path $PSScriptRoot 'jumpgate-web-logo.svg'

try {
  Render-Svg $splash (Join-Path $root 'media\applaunch_screen.png') 1920 1080
  $splashPng = Join-Path $root 'media\applaunch_screen.png'
  & ffmpeg -hide_banner -loglevel error -y -i $splashPng -q:v 2 (Join-Path $root 'media\splash.jpg')
  if ($LASTEXITCODE -ne 0) { throw 'Failed to create media/splash.jpg' }

  Render-Svg $wordmark (Join-Path $root 'media\vendor_logo.png') 465 128
  $androidWordmark = Join-Path $root `
    'tools\android\packaging\xbmc\res\drawable-nodpi\jumpgate_wordmark.png'
  New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($androidWordmark)) |
    Out-Null
  Copy-Item -LiteralPath (Join-Path $root 'media\vendor_logo.png') `
    -Destination $androidWordmark -Force
  Render-Svg $mark (Join-Path $root 'media\vendor_icon.png') 128 128

  foreach ($size in 16, 32, 48, 80, 120, 256) {
    Render-Svg $mark (Join-Path $root "media\icon${size}x${size}.png") $size $size
  }

  $androidMedia = Join-Path $root 'tools\android\packaging\media'
  Render-Svg $mark (Join-Path $androidMedia 'playstore.png') 512 512
  Render-Svg $mark (Join-Path $androidMedia 'mipmap-ldpi\ic_launcher.png') 36 36
  Render-Svg $mark (Join-Path $androidMedia 'mipmap-mdpi\ic_launcher.png') 48 48
  Render-Svg $mark (Join-Path $androidMedia 'mipmap-hdpi\ic_launcher.png') 72 72
  Render-Svg $mark (Join-Path $androidMedia 'mipmap-xhdpi\ic_launcher.png') 96 96
  Render-Svg $mark (Join-Path $androidMedia 'mipmap-xxhdpi\ic_launcher.png') 144 144
  Render-Svg $mark (Join-Path $androidMedia 'mipmap-xxxhdpi\ic_launcher.png') 192 192
  Render-Svg $banner (Join-Path $androidMedia 'mipmap-xhdpi\banner.png') 320 180

  Render-Svg $notification `
    (Join-Path $root 'tools\android\packaging\xbmc\res\drawable\notif_icon.png') 36 36
  Render-Svg $webLogo `
    (Join-Path $root 'addons\webinterface.default\themes\base\images\logo.png') 201 50
  foreach ($size in 32, 128, 144, 152, 192) {
    $name = if ($size -eq 32) { 'favicon.png' } else { "icon-$size.png" }
    Render-Svg $mark (Join-Path $root "addons\webinterface.default\$name") $size $size
  }
  Copy-Item -LiteralPath (Join-Path $root 'addons\webinterface.default\icon-144.png') `
    -Destination (Join-Path $root 'addons\webinterface.default\icon.png') -Force
  Render-Svg $wordmark `
    (Join-Path $root 'addons\webinterface.default\images\splash_hi.png') 465 128
}
finally {
  $temporaryRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
  $resolvedProfile = [System.IO.Path]::GetFullPath($chromeProfile)
  if ($resolvedProfile.StartsWith($temporaryRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    Remove-Item -LiteralPath $resolvedProfile -Recurse -Force -ErrorAction SilentlyContinue
  }
}

Write-Output 'Jumpgate brand assets rendered.'
