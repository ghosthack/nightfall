# input.ps1 hotkey | key | esc | click | move   (real input via keybd_event / mouse_event)
param([string]$What)
Add-Type @'
using System; using System.Runtime.InteropServices;
public class I {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
}
'@
$U = [UIntPtr]::Zero
function Down($vk) { [I]::keybd_event($vk, 0, 0, $U) }
function Up($vk)   { [I]::keybd_event($vk, 0, 2, $U) }
switch ($What) {
  'hotkey' { Down 0x11; Down 0x12; Down 0x42; Start-Sleep -m 50; Up 0x42; Up 0x12; Up 0x11 }
  'key'    { Down 0x20; Start-Sleep -m 50; Up 0x20 }
  'esc'    { Down 0x1B; Start-Sleep -m 50; Up 0x1B }
  'click'  { [I]::mouse_event(2,0,0,0,$U); Start-Sleep -m 50; [I]::mouse_event(4,0,0,0,$U) }
  'move'   { for ($i=0; $i -lt 20; $i++) { [I]::SetCursorPos(300 + $i*30, 300 + $i*10) | Out-Null; Start-Sleep -m 20 } }
}
