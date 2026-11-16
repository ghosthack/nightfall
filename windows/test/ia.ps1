# ia.ps1 "<command line>"  -> run it in the interactive (RDP) session of the current user and wait
param([string]$Cmd, [int]$Wait = 30)
$name = 'nightfall-test'
$a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command $Cmd"
$p = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $name -Action $a -Principal $p -Force | Out-Null
Start-ScheduledTask -TaskName $name
$t0 = Get-Date
do { Start-Sleep -m 300; $s = (Get-ScheduledTask -TaskName $name).State } while ($s -eq 'Running' -and ((Get-Date)-$t0).TotalSeconds -lt $Wait)
$r = (Get-ScheduledTaskInfo -TaskName $name).LastTaskResult
Unregister-ScheduledTask -TaskName $name -Confirm:$false
"task result: $r"
