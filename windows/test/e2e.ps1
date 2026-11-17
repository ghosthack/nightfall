# End-to-end test. Must run inside the interactive desktop session (run-e2e.ps1 arranges that).
# Uses real input (keybd_event/mouse_event) and real screenshots; results go to $env:TEMP\nightfall-e2e.
$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
$out = Join-Path $env:TEMP 'nightfall-e2e'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $out | Out-Null
$result = Join-Path $out 'result.txt'
function Say($m) { Add-Content $result $m }
$script:ok = 0; $script:fail = 0
function Check($name, [bool]$cond) {
    if ($cond) { Say "PASS $name"; $script:ok++ } else { Say "FAIL $name"; $script:fail++ }
}
Add-Type @'
using System; using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
}
'@
Add-Type -AssemblyName System.Drawing
function SaverUp { $h = [W]::FindWindow('NightfallSaver', [NullString]::Value); return ($h -ne [IntPtr]::Zero) -and [W]::IsWindowVisible($h) }
function SaverFocused { $h = [W]::FindWindow('NightfallSaver', [NullString]::Value); return ($h -ne [IntPtr]::Zero) -and ([W]::GetForegroundWindow() -eq $h) }
function Shot($name) { $p = Join-Path $out "$name.png"; & "$here\shot.ps1" $p; return $p }
# Samples a grid; returns @(fraction of non-black samples, signature string of lit cells)
function Analyze($png) {
    $b = [System.Drawing.Bitmap]::FromFile($png); $lit = 0; $n = 0; $sig = ''
    for ($y = 0; $y -lt 48; $y++) { for ($x = 0; $x -lt 64; $x++) {
        $c = $b.GetPixel([int](($x + 0.5) * $b.Width / 64), [int](($y + 0.5) * $b.Height / 48))
        $n++; if (($c.R + $c.G + $c.B) -gt 60) { $lit++; $sig += "$x,$y;" } } }
    $b.Dispose(); return @(($lit / $n), $sig)
}
function Input($what) { & "$here\input.ps1" $what | Out-Null }

$tray = Get-Process nightfall -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq (Get-Process -Id $PID).SessionId }
Check 'tray process running in this desktop session' ([bool]$tray)
if ($tray) { Say ("tray idle working set: {0} KB" -f [int]($tray[0].WorkingSet64 / 1KB)) }

# --- hotkey
Input hotkey; Start-Sleep 2
Check 'Ctrl+Alt+B shows the saver' (SaverUp)
Check 'saver has keyboard focus' (SaverFocused)
$a = Analyze (Shot 'blank-1')
Check 'screen is black apart from the clock (<5% lit, >0%)' (($a[0] -lt 0.05) -and ($a[0] -gt 0))
$p = Get-Process nightfall | Where-Object { $_.SessionId -eq (Get-Process -Id $PID).SessionId } | Select-Object -First 1
$cpu0 = $p.TotalProcessorTime
Input move; Start-Sleep 1
Check 'mouse movement does not wake' (SaverUp)
Start-Sleep 21
$b = Analyze (Shot 'blank-2')
Check 'clock moved after 20 s' ($a[1] -ne $b[1])
$p.Refresh()
Say ("while blanked: working set {0} KB, CPU {1} ms over ~22 s" -f [int]($p.WorkingSet64 / 1KB), [int]($p.TotalProcessorTime - $cpu0).TotalMilliseconds)
Input key; Start-Sleep 1
Check 'key wakes' (-not (SaverUp))
Check 'tray keeps running after wake' ([bool](Get-Process -Id $p.Id -ErrorAction SilentlyContinue))

# --- desktop shortcut (standalone /s), wake by click
$lnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Nightfall.lnk'
Check 'desktop shortcut exists' (Test-Path $lnk)
$before = @(Get-Process nightfall).Count
Start-Process $lnk; Start-Sleep 2
Check 'desktop shortcut shows the saver' (SaverUp)
Input click; Start-Sleep 1
Check 'mouse click wakes' (-not (SaverUp))
Start-Sleep 1
Check 'shortcut instance exits after wake' (@(Get-Process nightfall).Count -eq $before)

# --- digital rain: one-off /rain, then the saved style from the registry
$exe = Join-Path $env:LOCALAPPDATA 'Programs\Nightfall\nightfall.exe'
$r = Start-Process $exe -ArgumentList '/s', '/rain' -PassThru; Start-Sleep 3
Check '/rain shows the saver' (SaverUp)
$a = Analyze (Shot 'rain-1'); Start-Sleep -Milliseconds 700; $b = Analyze (Shot 'rain-2')
Check 'rain fills the screen with glyphs (>3% lit)' ($a[0] -gt 0.03)
Check 'rain is animating' ($a[1] -ne $b[1])
$c0 = $r.TotalProcessorTime; Start-Sleep 5; $r.Refresh()
Say ("rain: working set {0} KB, CPU {1:N1}% of one core" -f [int]($r.WorkingSet64 / 1KB), (($r.TotalProcessorTime - $c0).TotalMilliseconds / 50))
Input key; Start-Sleep 1
Check 'key wakes the rain' (-not (SaverUp))
$key = 'HKCU:\Software\Nightfall'
$old = (Get-ItemProperty $key -Name Mode -ErrorAction SilentlyContinue).Mode
if (-not (Test-Path $key)) { New-Item $key | Out-Null }
Set-ItemProperty $key -Name Mode -Value 1 -Type DWord
Start-Process $lnk; Start-Sleep 3
$a = Analyze (Shot 'rain-saved')
Check 'saved style (Mode=1) makes the shortcut use the rain' ((SaverUp) -and ($a[0] -gt 0.03))
Input key; Start-Sleep 1
if ($null -eq $old) { Remove-ItemProperty $key -Name Mode } else { Set-ItemProperty $key -Name Mode -Value $old -Type DWord }
Check 'style restored, rain closed' ((-not (SaverUp)) -and ((Get-ItemProperty $key -Name Mode -ErrorAction SilentlyContinue).Mode -eq $old))

# --- gray mosaic: one-off /mosaic
$m = Start-Process $exe -ArgumentList '/s', '/mosaic' -PassThru; Start-Sleep 3
Check '/mosaic shows the saver' (SaverUp)
$a = Analyze (Shot 'mosaic-1'); Start-Sleep 3; $b = Analyze (Shot 'mosaic-2')
Check 'mosaic covers the screen with gray cells (>30% lit)' ($a[0] -gt 0.3)
Check 'mosaic is changing' ($a[1] -ne $b[1])
$c0 = $m.TotalProcessorTime; Start-Sleep 5; $m.Refresh()
Say ("mosaic: working set {0} KB, CPU {1:N1}% of one core" -f [int]($m.WorkingSet64 / 1KB), (($m.TotalProcessorTime - $c0).TotalMilliseconds / 50))
Input key; Start-Sleep 1
Check 'key wakes the mosaic' (-not (SaverUp))

Say "RESULT: $script:ok passed, $script:fail failed"
