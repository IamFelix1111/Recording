# E2E regression suite for RecordingGui (refactor safety net).
# Usage: powershell -ExecutionPolicy Bypass -File tools/e2e.ps1 [-Build] [-StaticOnly]
# Exit code 0 = all green. Run before and after every refactor phase.

param(
    [string]$Exe,
    [switch]$Build,
    [switch]$StaticOnly
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Exe) { $Exe = Join-Path $root 'cmake-build-debug\RecordingGui.exe' }
$failures = 0

function Pass($name) { Write-Host "  [PASS] $name" -ForegroundColor Green }
function Fail($name, $detail = "") {
    Write-Host "  [FAIL] $name $detail" -ForegroundColor Red
    $script:failures++
}

if ($Build) {
    $mg = "C:\Users\Administrator\AppData\Local\Programs\CLion\bin\mingw\bin"
    $env:Path = "$mg;C:\Users\Administrator\AppData\Local\Programs\CLion\bin\cmake\win\x64\bin;C:\Users\Administrator\AppData\Local\Programs\CLion\bin\ninja\win\x64;$env:Path"
    Push-Location $root
    & cmake --build cmake-build-debug 2>&1 | Select-Object -Last 1 | Write-Host
    if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED"; exit 1 }
    Pop-Location
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class E2E {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int i);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string tit);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr m);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@

$script:appPid = [uint32]0

function Find-Top([string]$class, [switch]$VisibleOnly) {
    $script:found = [IntPtr]::Zero
    $cb = { param($h, $l)
        $wp = [uint32]0
        [void][E2E]::GetWindowThreadProcessId($h, [ref]$wp)
        if ($wp -eq $script:appPid) {
            if ($VisibleOnly -and -not [E2E]::IsWindowVisible($h)) { return $true }
            $c = New-Object System.Text.StringBuilder 64
            [void][E2E]::GetClassName($h, $c, 64)
            if ($c.ToString() -eq $class) { $script:found = $h; return $false }
        }
        return $true }
    [void][E2E]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

function WaitUntil([scriptblock]$cond, [int]$ms = 4000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $ms) {
        if (& $cond) { return $true }
        Start-Sleep -Milliseconds 50
    }
    return (& $cond)
}

function Get-StateColumn([IntPtr]$main) {
    $cb = { param($h, $l)
        $s = New-Object System.Text.StringBuilder 128
        [void][E2E]::GetWindowText($h, $s, 128)
        $t = $s.ToString()
        if ($t -in @("就绪","准备中","录制中","暂停","保存中","保存成功")) { $script:hit = $t; return $false }
        return $true }
    $script:hit = ""
    [void][E2E]::EnumChildWindows($main, $cb, [IntPtr]::Zero)
    return $script:hit
}

function Stop-App {
    Get-Process RecordingGui -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

# --- 0. static: no winpthread import ------------------------------------------
Write-Host "`n[0] 静态检查"
$bytes = [IO.File]::ReadAllBytes((Resolve-Path $Exe))
$ascii = [Text.Encoding]::ASCII.GetString($bytes)
if ($ascii.Contains("libwinpthread")) { Fail "winpthread 导入回归" } else { Pass "无 libwinpthread 导入" }

if ($StaticOnly) {
    if ($failures -eq 0) { Write-Host "`nALL GREEN (static only)" -ForegroundColor Green; exit 0 } else { exit 1 }
}

# --- launch -------------------------------------------------------------------
Stop-App
Start-Process $Exe -WorkingDirectory (Split-Path $Exe)
$proc = $null
$ok = WaitUntil { $script:p = Get-Process RecordingGui -ErrorAction SilentlyContinue; $script:p -and $script:p.MainWindowHandle -ne 0 }
if (-not $ok) { Fail "程序启动"; exit 1 }
$proc = $script:p
$script:appPid = [uint32]$proc.Id
$main = $proc.MainWindowHandle

# --- 1. ready state ------------------------------------------------------------
Write-Host "`n[1] 就绪状态"
if ((Get-StateColumn $main) -eq "就绪") { Pass "状态列=就绪" } else { Fail "状态列" ((Get-StateColumn $main)) }
$startBtn = [E2E]::GetDlgItem($main, 1008)
$fpsCombo = [E2E]::GetDlgItem($main, 1002)
if ([E2E]::IsWindowEnabled($startBtn)) { Pass "开始按钮可用" } else { Fail "开始按钮不可用" }

    # --- 2. record: state, controls, border, taskbar -------------------------------
Write-Host "`n[2] 录制"
$capDir0 = Join-Path ([Environment]::GetFolderPath('MyVideos')) 'Captures'
Get-ChildItem "$capDir0\Capture-*.mp4" -ErrorAction SilentlyContinue | Remove-Item
[void][E2E]::PostMessage($startBtn, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero)
$ok = WaitUntil { (Get-StateColumn $main) -eq "录制中" } 8000
if ($ok) { Pass "进入录制中" } else { Fail "进入录制中" ((Get-StateColumn $main)); Stop-App; exit 1 }
Start-Sleep -Milliseconds 800
if (-not [E2E]::IsWindowEnabled($fpsCombo)) { Pass "帧率框禁用" } else { Fail "帧率框应禁用" }
$chkCursor = [E2E]::GetDlgItem($main, 1006)
if ([E2E]::IsWindowEnabled($chkCursor)) { Pass "鼠标复选可用" } else { Fail "鼠标复选应可用" }

$script:strips = New-Object System.Collections.ArrayList
$cb = { param($h, $l)
    $wp = [uint32]0
    [void][E2E]::GetWindowThreadProcessId($h, [ref]$wp)
    if ($wp -eq $script:appPid) {
        $c = New-Object System.Text.StringBuilder 64
        [void][E2E]::GetClassName($h, $c, 64)
        if ($c.ToString() -eq "RecordingBorder") {
            $r = New-Object E2E+RECT
            [void][E2E]::GetWindowRect($h, [ref]$r)
            $ex = [E2E]::GetWindowLong($h, -20)
            [void]$script:strips.Add(@{R=$r; Topmost=(($ex -band 8) -ne 0)})
        }
    }
    return $true }
[void][E2E]::EnumWindows($cb, [IntPtr]::Zero)
if ($strips.Count -eq 4) { Pass "边框=4条" } else { Fail "边框条数" "=$($strips.Count)" }
if (($strips | Where-Object { -not $_.Topmost }).Count -eq 0) { Pass "边框全部置顶" } else { Fail "边框未全部置顶" }
$screenBottom = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Bottom
$maxB = ($strips | ForEach-Object { $_.R.B } | Measure-Object -Maximum).Maximum
if ($maxB -ge $screenBottom) { Pass "底边像素完整 (B=$maxB/$screenBottom)" } else { Fail "底边不完整" "B=$maxB < $screenBottom" }
$tb = [E2E]::FindWindow("Shell_TrayWnd", $null)
$tr = New-Object E2E+RECT
[void][E2E]::GetWindowRect($tb, [ref]$tr)
if ([E2E]::IsWindowVisible($tb) -and $tr.B -gt $tr.T) { Pass "任务栏可见未隐藏" } else { Fail "任务栏被隐藏" }

# --- 3. tray: hide window, menu, pause via click -------------------------------
Write-Host "`n[3] 托盘"
[void][E2E]::PostMessage($main, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # close=hide
Start-Sleep -Milliseconds 400
if (-not [E2E]::IsWindowVisible($main)) { Pass "关窗=隐藏" } else { Fail "关窗未隐藏" }
[void][E2E]::PostMessage($main, 0x8002, [IntPtr]1, [IntPtr]0x0205)       # tray right click
$menuH = [IntPtr]::Zero
$ok = WaitUntil {
    $script:mw = [IntPtr]::Zero
    $cbm = { param($h, $l)
        $wp = [uint32]0
        [void][E2E]::GetWindowThreadProcessId($h, [ref]$wp)
        if ($wp -eq $script:appPid) {
            $c = New-Object System.Text.StringBuilder 64
            [void][E2E]::GetClassName($h, $c, 64)
            if ($c.ToString() -eq "#32768") { $script:mw = $h; return $false }
        }
        return $true }
    [void][E2E]::EnumWindows($cbm, [IntPtr]::Zero)
    $script:mw -ne [IntPtr]::Zero
}
if ($ok) {
    $menuH = $script:mw
    $hmenu = [E2E]::SendMessage($menuH, 0x01E1, [IntPtr]::Zero, [IntPtr]::Zero)  # MN_GETHMENU
    if ([E2E]::GetMenuItemCount($hmenu) -eq 5) { Pass "菜单5项" } else { Fail "菜单项数" }
    Start-Sleep -Milliseconds 600
    if ([E2E]::IsWindowVisible($menuH)) { Pass "菜单存活>0.6s" } else { Fail "菜单秒关" }
    # TrackPopupMenu runs a MODAL loop on the GUI thread: dismiss it with ESC
    # (keyboard ends the menu loop) and WM_CANCELMODE on the owner, then wait.
    [void][E2E]::PostMessage($menuH, 0x0100, [IntPtr]0x1B, [IntPtr]::Zero)      # WM_KEYDOWN ESC
    [void][E2E]::PostMessage($main, 0x001B, [IntPtr]::Zero, [IntPtr]::Zero)     # WM_CANCELMODE to owner
    $gone = WaitUntil { -not [E2E]::IsWindowVisible($menuH) } 2000
    if ($gone) { Pass "菜单已关闭" } else {
        Fail "菜单未能关闭(模态阻塞)"
        [void][E2E]::PostMessage($menuH, 0x0100, [IntPtr]0x1B, [IntPtr]::0)
    }
} else { Fail "隐藏态右键菜单未出现" }
$dct = (Get-ItemProperty "HKCU:\Control Panel\Mouse").DoubleClickSpeed
Write-Host "    ..dct=$dct" 
[void][E2E]::PostMessage($main, 0x8002, [IntPtr]1, [IntPtr]0x0202)  # tray left up
Write-Host "    ..click1 posted"
Start-Sleep -Milliseconds ([int]$dct + 400)
Write-Host "    ..waited, state=$((Get-StateColumn $main))"
if ((Get-StateColumn $main) -eq "暂停") { Pass "托盘单击=暂停" } else { Fail "托盘单击暂停" ((Get-StateColumn $main)) }
[void][E2E]::PostMessage($main, 0x8002, [IntPtr]1, [IntPtr]0x0202)
Start-Sleep -Milliseconds ([int]$dct + 400)
if ((Get-StateColumn $main) -eq "录制中") { Pass "托盘单击=继续" } else { Fail "托盘单击继续" ((Get-StateColumn $main)) }
[void][E2E]::PostMessage($main, 0x0111, [IntPtr]2001, [IntPtr]::Zero)  # IDM_TRAY_SHOW
Start-Sleep -Milliseconds 400
if ([E2E]::IsWindowVisible($main)) { Pass "托盘菜单显示界面" } else { Fail "显示界面未生效" }

# --- 4. stop: save + file + border teardown ------------------------------------
Write-Host "`n[4] 停止"
$capDir = Join-Path ([Environment]::GetFolderPath('MyVideos')) 'Captures'
[void][E2E]::PostMessage($startBtn, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero)
$ok = WaitUntil { (Get-StateColumn $main) -eq "保存成功" } 8000
if ($ok) { Pass "保存成功" } else {
    Fail "保存" ((Get-StateColumn $main))
    # Diagnostics: an ownerless modal from showError() freezes the UI thread.
    $script:dlgs = New-Object System.Collections.ArrayList
    $cbd = { param($h, $l)
        $wp = [uint32]0
        [void][E2E]::GetWindowThreadProcessId($h, [ref]$wp)
        if ($wp -eq $script:appPid) {
            $c = New-Object System.Text.StringBuilder 64
            [void][E2E]::GetClassName($h, $c, 64)
            if ($c.ToString() -eq "#32770") {
                $t = New-Object System.Text.StringBuilder 512
                [void][E2E]::GetWindowText($h, $t, 512)
                [void]$script:dlgs.Add($t.ToString())
            }
        }
        return $true }
    [void][E2E]::EnumWindows($cbd, [IntPtr]::Zero)
    if ($dlgs.Count -gt 0) {
        Write-Host "    模态框阻塞: $($dlgs -join ' | ')" -ForegroundColor Yellow
        # Dump the message body (STATIC children of the dialog).
        $script:dd = [IntPtr]::Zero
        $cbx = { param($h, $l)
            $wp = [uint32]0
            [void][E2E]::GetWindowThreadProcessId($h, [ref]$wp)
            if ($wp -eq $script:appPid) {
                $c = New-Object System.Text.StringBuilder 64
                [void][E2E]::GetClassName($h, $c, 64)
                if ($c.ToString() -eq "#32770") { $script:dd = $h; return $false }
            }
            return $true }
        [void][E2E]::EnumWindows($cbx, [IntPtr]::Zero)
        if ($script:dd -ne [IntPtr]::Zero) {
            $script:body = New-Object System.Collections.ArrayList
            $cbc = { param($h, $l)
                $s = New-Object System.Text.StringBuilder 512
                [void][E2E]::GetWindowText($h, $s, 512)
                $t = $s.ToString()
                if ($t -ne "" -and $t -notmatch '^(确定|取消|是|否|错误)$') { [void]$script:body.Add($t) }
                return $true }
            [void][E2E]::EnumChildWindows($script:dd, $cbc, [IntPtr]::Zero)
            if ($body.Count -gt 0) { Write-Host "    错误正文: $($body -join ' | ')" -ForegroundColor Yellow }
            $okBtn = [E2E]::GetDlgItem($script:dd, 2)  # IDCANCEL
            if ($okBtn -eq [IntPtr]::Zero) { $okBtn = [E2E]::GetDlgItem($script:dd, 1) }
            if ($okBtn -ne [IntPtr]::Zero) { [void][E2E]::PostMessage($okBtn, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) }
        }
        Start-Sleep -Milliseconds 600
    }
}
Start-Sleep -Milliseconds 400
$file = Get-ChildItem "$capDir\Capture-*.mp4" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($file -and $file.Length -gt 10KB) { Pass "文件落盘 ($([math]::Round($file.Length/1KB))KB)" } else { Fail "文件未落盘" }
if ((Find-Top "RecordingBorder") -eq [IntPtr]::Zero) { Pass "边框已拆除" } else { Fail "边框残留" }
if ($file) { Remove-Item $file.FullName -ErrorAction SilentlyContinue }

# --- 5. ready close = quit ------------------------------------------------------
Write-Host "`n[5] 退出"
[void][E2E]::PostMessage($main, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
$ok = WaitUntil { -not (Get-Process -Id $script:appPid -ErrorAction SilentlyContinue) } 3000
if ($ok) { Pass "Ready 关窗退出" } else { Fail "Ready 关窗未退出" }

Stop-App
if ($failures -eq 0) { Write-Host "`n===== ALL GREEN =====" -ForegroundColor Green; exit 0 }
Write-Host "`n===== $failures FAILED =====" -ForegroundColor Red; exit 1
