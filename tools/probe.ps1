param(
    [string]$Action = "info",
    [string]$Out = "C:\Users\xmyci\AppData\Local\Temp\opencode\probe.png"
)

# 调试助手：在不依赖桌面截图（会被 DPI 缩放干扰）的情况下，
# 直接对任务栏控件/弹窗做 PrintWindow 抓图。
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Probe {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public static IntPtr FindChild(string cls) {
    IntPtr found = IntPtr.Zero;
    IntPtr tray = FindWindow("Shell_TrayWnd", null);
    EnumChildWindows(tray, (h,l) => { var c=new StringBuilder(256); GetClassName(h,c,256); if (c.ToString()==cls){found=h;return false;} return true; }, IntPtr.Zero);
    return found;
  }
  public static IntPtr FindTop(uint pid, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h,l) => { uint p; GetWindowThreadProcessId(h, out p); if (p!=pid) return true;
      var c=new StringBuilder(256); GetClassName(h,c,256); if (c.ToString()==cls){found=h;return false;} return true; }, IntPtr.Zero);
    return found;
  }
}
"@

$proc = Get-Process orbital_freight_taskbar -ErrorAction SilentlyContinue
if (-not $proc) { Write-Output "app not running"; exit 1 }

# 让探测进程本身 DPI 感知，否则 Windows 会把窗口尺寸/坐标虚拟化（150% 缩放下相差 1.5 倍）。
[Probe]::SetProcessDpiAwarenessContext([IntPtr]-4) | Out-Null

$has_popup = $false

$widget = [Probe]::FindChild("OrbitalFreightWidgetWindow")
$popup = [Probe]::FindTop([uint32]$proc.Id, "OrbitalFreightPopupWindow")
$has_popup = $popup -ne [IntPtr]::Zero

switch ($Action) {
    "toggle" {
        $ctrl = [Probe]::FindTop([uint32]$proc.Id, "OrbitalFreightControllerWindow")
        [Probe]::PostMessage($ctrl, 0x0312, [IntPtr]1, [IntPtr]0) | Out-Null
        Start-Sleep -Milliseconds 800
        Write-Output "toggled"
    }
    "open" {
        [Probe]::PostMessage($widget, 0x0201, [IntPtr]0, [IntPtr]0) | Out-Null
        [Probe]::PostMessage($widget, 0x0202, [IntPtr]0, [IntPtr]0) | Out-Null
        Start-Sleep -Milliseconds 800
        Write-Output "opened"
    }
    "close" {
        [Probe]::PostMessage($popup, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
        Write-Output "closed"
    }
    "restart" {
        $ctrl = [Probe]::FindTop([uint32]$proc.Id, "OrbitalFreightControllerWindow")
        [Probe]::PostMessage($ctrl, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
        Start-Sleep -Seconds 2
        $exe = Join-Path $PSScriptRoot "..\build\Release\orbital_freight_taskbar.exe"
        Start-Process -FilePath $exe
        Start-Sleep -Seconds 3
        Write-Output "restarted"
    }
    "key" {
        if (-not $has_popup) { Write-Output "no popup"; exit 1 }
        [Probe]::PostMessage($popup, 0x0100, [IntPtr][int]$Out, [IntPtr]0) | Out-Null
        Start-Sleep -Milliseconds 400
        Write-Output "key sent: $Out"
    }
    "shot" {
        $target = if ($has_popup) { $popup } else { $widget }
        if ($target -eq [IntPtr]::Zero) { Write-Output "no window"; exit 1 }
        $r = New-Object Probe+RECT
        [Probe]::GetWindowRect($target, [ref]$r) | Out-Null
        $dpi = [Probe]::GetDpiForWindow($target)
        $w = $r.Right - $r.Left
        $h = $r.Bottom - $r.Top
        if ($w -le 0 -or $h -le 0) { Write-Output "bad rect"; exit 1 }
        Add-Type -AssemblyName System.Drawing
        $bmp = New-Object System.Drawing.Bitmap $w, $h
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc()
        [Probe]::PrintWindow($target, $hdc, 2) | Out-Null
        $g.ReleaseHdc($hdc)
        $g.Dispose()
        $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        Write-Output "saved $Out ($w x $h, dpi=$dpi)"
    }
    default {
        Write-Output "widget=$widget popup=$popup"
    }
}
