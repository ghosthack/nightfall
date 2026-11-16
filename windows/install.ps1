# Installs Nightfall for the current user (no admin needed).
#   - copies nightfall.exe to %LOCALAPPDATA%\Programs\Nightfall
#   - Desktop + Start Menu shortcut "Nightfall"        -> blank the screen now
#   - Start Menu shortcut "Nightfall (tray)"            -> tray icon + Ctrl+Alt+B hotkey
#   - tray mode starts with Windows (HKCU Run), toggle from the tray menu
param([switch]$NoAutostart, [switch]$NoLaunch)
$ErrorActionPreference = 'Stop'
$src  = Join-Path $PSScriptRoot 'nightfall.exe'
if (-not (Test-Path $src)) { $src = Join-Path $PSScriptRoot 'build\nightfall.exe' }
$dir  = Join-Path $env:LOCALAPPDATA 'Programs\Nightfall'
$exe  = Join-Path $dir 'nightfall.exe'

Get-Process nightfall -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item $src $exe -Force

$sh = New-Object -ComObject WScript.Shell
function New-Link($path, $arguments, $desc) {
    $l = $sh.CreateShortcut($path)
    $l.TargetPath = $exe; $l.Arguments = $arguments; $l.WorkingDirectory = $dir
    $l.IconLocation = "$exe,0"; $l.Description = $desc
    $l.Save()
}
$desktop = [Environment]::GetFolderPath('Desktop')
$menu    = Join-Path ([Environment]::GetFolderPath('Programs')) 'Nightfall'
New-Item -ItemType Directory -Force $menu | Out-Null
New-Link (Join-Path $desktop 'Nightfall.lnk')          '/s'    'Blank the screen now (any key or click to wake)'
New-Link (Join-Path $menu    'Nightfall.lnk')          '/s'    'Blank the screen now (any key or click to wake)'
New-Link (Join-Path $menu    'Nightfall (tray).lnk')   '/tray' 'Tray icon: click the moon or press Ctrl+Alt+B'

$run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
if ($NoAutostart) { Remove-ItemProperty $run -Name Nightfall -ErrorAction SilentlyContinue }
else { Set-ItemProperty $run -Name Nightfall -Value "`"$exe`" /tray" }

if (-not $NoLaunch) {
    Start-Process $exe -ArgumentList '/tray'
    # Windows 11 hides new tray icons in the overflow; pin ours next to the clock.
    Start-Sleep 2
    Get-ChildItem 'HKCU:\Control Panel\NotifyIconSettings' -ErrorAction SilentlyContinue |
        Where-Object { (Get-ItemProperty $_.PSPath).ExecutablePath -eq $exe } |
        ForEach-Object { Set-ItemProperty $_.PSPath -Name IsPromoted -Value 1 -Type DWord }
}
"Installed to $exe"
