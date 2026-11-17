# Run from SSH (or any non-interactive shell): executes e2e.ps1 inside the logged-in desktop
# session via a one-shot scheduled task, then prints the results.
$here = $PSScriptRoot
& "$here\ia.ps1" -Cmd "& '$here\e2e.ps1'" -Wait 300 | Out-Null
Get-Content (Join-Path $env:TEMP 'nightfall-e2e\result.txt')
