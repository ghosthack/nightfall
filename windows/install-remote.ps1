# Run from SSH: installs into the logged-in desktop session, so the tray icon starts there
# (a process started directly from SSH would land in the SSH session, invisible to the user).
$log = Join-Path $env:TEMP 'nightfall-install.log'
Remove-Item $log -ErrorAction SilentlyContinue
& "$PSScriptRoot\test\ia.ps1" -Cmd "& '$PSScriptRoot\install.ps1' *> '$log'" -Wait 60 | Out-Null
Get-Content $log -ErrorAction SilentlyContinue
