# Acceptance check for the native Windows ARM64 runtime.
#
# Passes only when, after a full Android boot on the native (non-WSL) runtime, the
# QEMU window actually shows the post-boot UI rather than the frozen boot animation.
# Exit 0 = the UI is visible, 10 = visible but the guest fell back off virgl,
# 1 = UI not visible, 2 = actual window presentation could not be verified.
[CmdletBinding()]
param(
    [string]$Runtime,
    [string]$SmokeApk,
    [int]$Cpus = 4,
    [int]$MemoryMB = 6144,
    [int]$BootTimeoutSeconds = 420,
    [string]$AnalyzeOnly,
    [string]$CapturePath,
    [switch]$KeepRunning
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new()

# A failure in the harness itself (window missing, capture blocked by a locked or
# covered session) must not look like the display verdict; exit 2 as documented.
trap {
    Write-Host "Harness error: $_"
    Restore-TestSettings
    exit 2
}

function Get-Python {
    foreach ($candidate in @('C:\Python314\python.exe', 'C:\msys64\ucrt64\bin\python.exe')) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw 'No native Windows Python found.'
}

function Get-Adb {
    $adb = Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'
    if (-not (Test-Path -LiteralPath $adb)) { throw "adb not found: $adb" }
    return $adb
}

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class NativeFrame {
  public delegate bool EnumCallback(IntPtr hwnd, IntPtr data);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumCallback callback, IntPtr data);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int max);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder text, int max);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr context);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hwnd);
  [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(Point point);
  [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int cmd);
  public const int SW_RESTORE = 9;
  // PW_RENDERFULLCONTENT: ask DWM for the window's own composited pixels.
  public const uint PW_RENDERFULLCONTENT = 2;
  public struct Rect { public int Left, Top, Right, Bottom; }
  public struct Point { public int X, Y; }
  public const uint GA_ROOT = 2;
  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
  static readonly IntPtr PerMonitorAwareV2 = new IntPtr(-4);
  public static bool DpiAware = false;
  // Must run before any DPI-dependent Win32 call. Without it a DPI-unaware
  // process gets virtualised (logical) GetWindowRect values while the screen DC
  // still yields physical pixels, so CopyFromScreen crops the wrong region
  // (measured 1.5x on this box at 150% scaling).
  public static void EnsureDpiAware() {
    if (DpiAware) return;
    try { DpiAware = SetProcessDpiAwarenessContext(PerMonitorAwareV2); }
    catch { DpiAware = false; }
    if (!DpiAware) { DpiAware = SetProcessDPIAware(); }
  }
  // Root window actually stacked at a screen point, so a capture can refuse to
  // measure whatever happens to be covering the QEMU window.
  public static IntPtr TopWindowAt(int x, int y) {
    var point = new Point(); point.X = x; point.Y = y;
    var hwnd = WindowFromPoint(point);
    if (hwnd == IntPtr.Zero) return IntPtr.Zero;
    var root = GetAncestor(hwnd, GA_ROOT);
    return root == IntPtr.Zero ? hwnd : root;
  }
  public static string Describe(IntPtr hwnd) {
    if (hwnd == IntPtr.Zero) return "(none)";
    var title = new StringBuilder(512); GetWindowText(hwnd, title, title.Capacity);
    var cls = new StringBuilder(512); GetClassName(hwnd, cls, cls.Capacity);
    return "class='" + cls + "' title='" + title + "'";
  }
  // A locked session puts a full-screen lock-screen backstop above everything,
  // so no screen capture can see the QEMU window until the desktop is unlocked.
  public static bool IsLockScreen() {
    bool locked = false;
    EnumWindows((hwnd, data) => {
      if (!IsWindowVisible(hwnd)) return true;
      var rect = new Rect();
      if (!GetWindowRect(hwnd, out rect)) return true;
      if (rect.Right - rect.Left < 1000 || rect.Bottom - rect.Top < 700) return true;
      var title = new StringBuilder(512); GetWindowText(hwnd, title, title.Capacity);
      var cls = new StringBuilder(512); GetClassName(hwnd, cls, cls.Capacity);
      if (cls.ToString().IndexOf("LockScreen", StringComparison.OrdinalIgnoreCase) >= 0 ||
          title.ToString().IndexOf("Backstop", StringComparison.OrdinalIgnoreCase) >= 0 ||
          title.ToString().IndexOf("锁屏", StringComparison.Ordinal) >= 0) {
        locked = true;
        return false;
      }
      return true;
    }, IntPtr.Zero);
    return locked;
  }
  public static IntPtr Find(string needle) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((hwnd, data) => {
      if (!IsWindowVisible(hwnd)) return true;
      var text = new StringBuilder(512);
      GetWindowText(hwnd, text, text.Capacity);
      if (text.ToString().IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0) { found = hwnd; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
'@

function Measure-Frame {
    param([string]$Path)
    $image = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        $step = [Math]::Max(1, [int]($image.Width / 220))
        $sampled = 0; $appPixels = 0; $bluePixels = 0; $darkPixels = 0
        for ($y = 0; $y -lt $image.Height; $y += $step) {
            for ($x = 0; $x -lt $image.Width; $x += $step) {
                $c = $image.GetPixel($x, $y)
                $sampled++
                # The smoke-test activity paints #EDF5EF over almost the whole screen.
                if ([Math]::Abs($c.R - 237) -le 26 -and [Math]::Abs($c.G - 245) -le 26 -and [Math]::Abs($c.B - 239) -le 26) { $appPixels++ }
                if ($c.B -gt ($c.R + 20) -and $c.B -gt ($c.G + 10)) { $bluePixels++ }
                if ((($c.R + $c.G + $c.B) / 3) -lt 30) { $darkPixels++ }
            }
        }
        return [pscustomobject]@{
            Width = $image.Width; Height = $image.Height; Sampled = $sampled
            AppFraction = [Math]::Round($appPixels / [Math]::Max(1, $sampled), 4)
            BlueFraction = [Math]::Round($bluePixels / [Math]::Max(1, $sampled), 4)
            DarkFraction = [Math]::Round($darkPixels / [Math]::Max(1, $sampled), 4)
        }
    } finally { $image.Dispose() }
}

function Capture-NativeWindow {
    param([string]$Path)
    # GetWindowRect returns physical pixels only once this process is DPI aware;
    # otherwise it returns virtualised (logical) values while the screen DC still
    # yields physical pixels, so a screen capture crops the wrong region
    # (measured 1.5x on this box at 150% scaling).
    [NativeFrame]::EnsureDpiAware()
    $hwnd = [NativeFrame]::Find('Tebox ARM64')
    if ($hwnd -eq [IntPtr]::Zero) { throw 'The native QEMU window is not present.' }
    if ([NativeFrame]::IsIconic($hwnd)) { [NativeFrame]::ShowWindow($hwnd, [NativeFrame]::SW_RESTORE) | Out-Null; Start-Sleep -Milliseconds 800 }
    $HWND_TOPMOST = [IntPtr](-1); $SWP_NOSIZE = 0x1; $SWP_NOMOVE = 0x2; $SWP_SHOWWINDOW = 0x40
    [NativeFrame]::SetWindowPos($hwnd, $HWND_TOPMOST, 0, 0, 0, 0, $SWP_NOSIZE -bor $SWP_NOMOVE -bor $SWP_SHOWWINDOW) | Out-Null
    Start-Sleep -Milliseconds 1200
    $rect = New-Object NativeFrame+Rect
    [NativeFrame]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
    $width = $rect.Right - $rect.Left; $height = $rect.Bottom - $rect.Top
    if ($width -lt 100 -or $height -lt 100) { throw "QEMU window is too small to capture: ${width}x${height}" }
    # The measurement has to be of what is actually on screen. PrintWindow is
    # not faithful for this SDL/GL window: it returned an all-black frame while
    # the window's own front buffer held the Android UI (read back inside QEMU).
    # Screen scraping only reads the QEMU window when nothing is stacked over it,
    # so refuse to judge when a locked session or another window covers it.
    $cx = [int](($rect.Left + $rect.Right) / 2); $cy = [int](($rect.Top + $rect.Bottom) / 2)
    $top = [NativeFrame]::TopWindowAt($cx, $cy)
    if ($top -ne $hwnd) {
        if ([NativeFrame]::IsLockScreen()) {
            throw 'The Windows desktop is locked; a screen capture would read the lock screen, not the QEMU window. Unlock the desktop and re-run.'
        }
        throw "The QEMU window is covered at (${cx},${cy}) by $([NativeFrame]::Describe($top)); bring it to the front and re-run."
    }
    Write-Host ("Capture: screen dpi={0} aware={1} rect={2},{3} {4}x{5}" -f `
        [NativeFrame]::GetDpiForWindow($hwnd), [NativeFrame]::DpiAware, $rect.Left, $rect.Top, $width, $height)
    $bitmap = New-Object Drawing.Bitmap($width, $height)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object Drawing.Size($width, $height)))
    $graphics.Dispose()
    $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    $HWND_NOTOPMOST = [IntPtr](-2)
    [NativeFrame]::SetWindowPos($hwnd, $HWND_NOTOPMOST, 0, 0, 0, 0, $SWP_NOSIZE -bor $SWP_NOMOVE) | Out-Null
}

function Test-NativeWindowVisible {
    # The QEMU window handle when it is the top window at its centre, else
    # $null. A locked desktop puts the lock-screen backstop above everything,
    # where no screen capture can see the QEMU window.
    [NativeFrame]::EnsureDpiAware()
    $hwnd = [NativeFrame]::Find('Tebox ARM64')
    if ($hwnd -eq [IntPtr]::Zero) { return $null }
    $rect = New-Object NativeFrame+Rect
    [NativeFrame]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
    $cx = [int](($rect.Left + $rect.Right) / 2); $cy = [int](($rect.Top + $rect.Bottom) / 2)
    if ([NativeFrame]::TopWindowAt($cx, $cy) -ne $hwnd) { return $null }
    return $hwnd
}

function Get-ScanoutProbeSample {
    # Last front-buffer readback QEMU logged for this run. This is the only
    # measurement available when the Windows desktop is locked; it needs the
    # staged QEMU built with TEBOX_SCANOUT_PROBE=1 in its environment.
    param([string]$LogPath)
    if (-not (Test-Path -LiteralPath $LogPath)) { return $null }
    $line = Get-Content -LiteralPath $LogPath -Tail 4000 |
            Where-Object { $_ -match 'stage=window_front_after_swap' } | Select-Object -Last 1
    if (-not $line) { return $null }
    $crc = [regex]::Match($line, 'crc=([0-9a-fA-F]+)').Groups[1].Value
    if (-not $crc) { return $null }
    $colors = [regex]::Match($line, 'colors=([0-9,;]+)').Groups[1].Value
    $appUi = $false
    foreach ($triple in ($colors -split ';')) {
        $parts = $triple -split ','
        if ($parts.Count -eq 3) {
            if ([Math]::Abs([int]$parts[0] - 237) -le 26 -and [Math]::Abs([int]$parts[1] - 245) -le 26 `
                -and [Math]::Abs([int]$parts[2] - 239) -le 26) { $appUi = $true }
        }
    }
    return [pscustomobject]@{
        Seq = [regex]::Match($line, 'seq=(\d+)').Groups[1].Value
        Crc = $crc; Colors = $colors; AppUi = $appUi; Line = $line.Trim()
    }
}

function Report {
    param([pscustomobject]$Frame, [string]$Renderer, [string]$Verdict, [int]$ExitCode,
          [string]$Path = 'n/a', [string]$ScreenPath = 'n/a', [string]$Desktop = 'n/a',
          [string]$UserVisible = 'not-covered')
    [pscustomobject]@{
        verdict = $Verdict
        appFraction = $Frame.AppFraction
        blueFraction = $Frame.BlueFraction
        darkFraction = $Frame.DarkFraction
        window = "$($Frame.Width)x$($Frame.Height)"
        renderer = $Renderer
        capture = $CapturePath
        path = $Path
        screenCapture = $ScreenPath
        desktop = $Desktop
        userVisible = $UserVisible
    } | ConvertTo-Json -Depth 4
    Restore-TestSettings
    exit $ExitCode
}

$script:SavedScreenTimeout = $null
$script:SavedStayOn = $null
$script:TestStarted = $false
function Restore-TestSettings {
    if ($null -ne $script:SavedScreenTimeout) {
        if ($script:SavedScreenTimeout -eq 'null') { & $adb -s $target shell settings delete system screen_off_timeout | Out-Null }
        else { & $adb -s $target shell settings put system screen_off_timeout $script:SavedScreenTimeout | Out-Null }
    }
    if ($null -ne $script:SavedStayOn) {
        if ($script:SavedStayOn -eq 'null') { & $adb -s $target shell settings delete global stay_on_while_plugged_in | Out-Null }
        else { & $adb -s $target shell settings put global stay_on_while_plugged_in $script:SavedStayOn | Out-Null }
    }
    $script:SavedScreenTimeout = $null
    $script:SavedStayOn = $null
    if ($script:TestStarted -and -not $KeepRunning) {
        & $python $manager stop --runtime $Runtime | Out-Null
        $script:TestStarted = $false
    }
}

if ($AnalyzeOnly) {
    if (-not (Test-Path -LiteralPath $AnalyzeOnly)) { throw "No such capture: $AnalyzeOnly" }
    $frame = Measure-Frame -Path $AnalyzeOnly
    $visible = $frame.AppFraction -ge 0.5
    Report -Frame $frame -Renderer 'n/a' -Verdict ($(if ($visible) { 'ui-visible' } else { 'ui-not-visible' })) `
           -ExitCode ($(if ($visible) { 0 } else { 1 }))
}

if (-not $Runtime -or -not $SmokeApk) { throw 'Provide a dedicated --Runtime and -SmokeApk before testing.' }
$Runtime = [IO.Path]::GetFullPath($Runtime)
if ($Runtime.TrimEnd('\') -eq ([IO.Path]::GetFullPath("$env:LOCALAPPDATA\TeboxNative")).TrimEnd('\')) {
    throw 'Use a separate test runtime; the normal runtime is protected.'
}
if (-not (Test-Path -LiteralPath $SmokeApk -PathType Leaf)) { throw "Smoke APK missing: $SmokeApk" }
$testProfile = Get-Content -LiteralPath (Join-Path $Runtime 'native-profile.json') -Raw | ConvertFrom-Json
foreach ($port in @($testProfile.adb_port, $testProfile.qmp_port, $testProfile.console_port)) {
    if ($port -in @(5700,5701,5702)) { throw 'Test profile must use ports distinct from the normal runtime.' }
    if (Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue) { throw "Test port already occupied: $port" }
}
$python = Get-Python
$manager = Join-Path $PSScriptRoot 'manage-native.py'
if (-not (Test-Path -LiteralPath $manager)) { throw "Missing native launcher: $manager" }
& $python -c "import importlib.util,sys,pathlib; s=importlib.util.spec_from_file_location('m',sys.argv[1]); m=importlib.util.module_from_spec(s); s.loader.exec_module(m); sys.exit(1 if m.native_pid(pathlib.Path(sys.argv[2])/'run') else 0)" $manager $Runtime
if ($LASTEXITCODE -ne 0) { throw 'Test runtime already running; refusing to restart it.' }

if (-not $CapturePath) { $CapturePath = Join-Path $env:TEMP 'native-acceptance.png' }

# The staged QEMU reads its scanout back only when this is set; it is inert
# otherwise, so the normal presentation path is unchanged.
$env:TEBOX_SCANOUT_PROBE = '1'

Write-Host "Booting the native VM (up to ${BootTimeoutSeconds}s)..."
$script:TestStarted = $true
& $python $manager start --runtime $Runtime --cpus $Cpus --memory $MemoryMB --wait $BootTimeoutSeconds
if ($LASTEXITCODE -ne 0) { throw 'The native VM did not reach a stable Android boot.' }

$profile = Get-Content -LiteralPath (Join-Path $Runtime 'native-profile.json') -Raw | ConvertFrom-Json
$adb = Get-Adb
$target = "127.0.0.1:$($profile.adb_port)"
& $adb connect $target | Out-Null
$renderer = (& $adb -s $target shell 'dumpsys SurfaceFlinger | grep GLES' 2>&1 | Out-String).Trim()
Write-Host "Guest renderer: $renderer"

$package = 'local.tebox.smoketest'
$installed = (& $adb -s $target shell "pm list packages $package" 2>&1 | Out-String)
if ($installed -notmatch [regex]::Escape($package)) {
    $apk = $SmokeApk
    if (-not (Test-Path -LiteralPath $apk)) { throw "Smoke APK missing: $apk" }
    & $adb -s $target install --no-incremental $apk | Out-Null
}
$script:SavedScreenTimeout = (& $adb -s $target shell settings get system screen_off_timeout | Out-String).Trim()
$script:SavedStayOn = (& $adb -s $target shell settings get global stay_on_while_plugged_in | Out-String).Trim()
& $adb -s $target shell 'settings put system screen_off_timeout 3600000' | Out-Null
& $adb -s $target shell 'svc power stayon true' | Out-Null
& $adb -s $target shell 'input keyevent KEYCODE_WAKEUP' | Out-Null
& $adb -s $target shell 'wm dismiss-keyguard' | Out-Null
Start-Sleep -Seconds 1
& $adb -s $target shell 'input swipe 540 2000 540 400 600' | Out-Null
Start-Sleep -Seconds 2
& $adb -s $target shell 'am start -W -n local.tebox.smoketest/.MainActivity' | Out-Null
Start-Sleep -Seconds 4
# The guest only paints the UI while its own display is on and unlocked, so a
# dark capture is only meaningful when this says the display is awake.
$wake = (& $adb -s $target shell 'dumpsys power | grep Wakefulness' 2>&1 | Out-String).Trim()
$keyguard = (& $adb -s $target shell 'dumpsys window | grep -i keyguard' 2>&1 | Out-String).Trim()
Write-Host "Guest display: $wake"
Write-Host "Guest keyguard: $keyguard"

$gpu = $renderer -match 'virgl'
$logPath = Join-Path $Runtime 'run/qemu.log'
$desktop = if ([NativeFrame]::IsLockScreen()) { 'locked' } else { 'unlocked-or-covered' }
$frame = $null
$path = 'n/a'
$screenPath = 'n/a'

# Path 1: a real screen capture, preferred while the desktop is unlocked.
if (Test-NativeWindowVisible) {
    try {
        Capture-NativeWindow -Path $CapturePath
        $frame = Measure-Frame -Path $CapturePath
        $path = 'screen-capture'
        $screenPath = 'available'
        Write-Host ("Frame: app={0} blue={1} dark={2} window={3}x{4}" -f `
            $frame.AppFraction, $frame.BlueFraction, $frame.DarkFraction, $frame.Width, $frame.Height)
    } catch {
        $screenPath = "unavailable: $_"
        Write-Host "Screen capture unavailable: $_"
    }
} else {
    $screenPath = 'unavailable: the QEMU window is not the top window (desktop may be locked)'
    Write-Host "Screen capture unavailable: $screenPath"
}

# Path 2: QEMU's own scanout readback. A host capture of this SDL/GL window is
# not faithful on this host: PrintWindow returns an all-black client area
# (measured app=0.0131 dark=0.9864 while the window's own front buffer held the
# UI) and a screen capture is blocked whenever the desktop is locked. The window's
# own front buffer is diagnostic evidence only: it does not establish that DWM
# presents those pixels. A missing or failed screen capture cannot be promoted
# to an acceptance pass by internal readback, even when that readback changes.
if (-not $frame -or $frame.AppFraction -lt 0.5) {
    $path = 'scanout-readback'
    $sampleA = Get-ScanoutProbeSample -LogPath $logPath
    if (-not $sampleA) {
        Restore-TestSettings
        throw "No scanout readback in $logPath; the staged QEMU was not built with the TEBOX_SCANOUT_PROBE readback."
    }
    & $adb -s $target shell 'am force-stop local.tebox.smoketest' | Out-Null
    Start-Sleep -Seconds 5
    $sampleB = Get-ScanoutProbeSample -LogPath $logPath
    Write-Host "Readback app visible: $($sampleA.Line)"
    Write-Host "Readback after change: $(if ($sampleB) { $sampleB.Line } else { '(none)' })"
    $frame = [pscustomobject]@{
        Width = $(if ($frame) { $frame.Width } else { 0 })
        Height = $(if ($frame) { $frame.Height } else { 0 })
        Sampled = 0
        AppFraction = $(if ($sampleA.AppUi) { 1 } else { 0 })
        BlueFraction = 0
        DarkFraction = $(if ($sampleA.AppUi) { 0 } else { 1 })
    }
    Restore-TestSettings
    if (-not $sampleA.AppUi) {
        Report -Frame $frame -Renderer $renderer -Verdict 'scanout-readback-shows-something-else' -ExitCode 1 `
               -Path $path -ScreenPath $screenPath -Desktop $desktop -UserVisible 'not-covered'
    }
    if (-not $sampleB -or $sampleA.Crc -eq $sampleB.Crc) {
        Report -Frame $frame -Renderer $renderer -Verdict 'scanout-readback-frozen' -ExitCode 1 `
               -Path $path -ScreenPath $screenPath -Desktop $desktop -UserVisible 'not-covered'
    }
    if ($gpu) {
        $presentationExit = if ($screenPath -eq 'available') { 1 } else { 2 }
        Report -Frame $frame -Renderer $renderer -Verdict 'internal-rendering-live-window-presentation-unverified' -ExitCode $presentationExit `
               -Path $path -ScreenPath $screenPath -Desktop $desktop -UserVisible 'not-covered'
    }
    Report -Frame $frame -Renderer $renderer -Verdict 'internal-software-rendering-window-presentation-unverified' -ExitCode 2 `
           -Path $path -ScreenPath $screenPath -Desktop $desktop -UserVisible 'not-covered'
}

Restore-TestSettings

if ($frame.AppFraction -ge 0.5) {
    if ($gpu) { Report -Frame $frame -Renderer $renderer -Verdict 'ui-visible-with-virgl' -ExitCode 0 `
                -Path $path -ScreenPath $screenPath -Desktop $desktop -UserVisible 'not-covered' }
    Report -Frame $frame -Renderer $renderer -Verdict 'ui-visible-but-software-rendering' -ExitCode 10 `
           -Path $path -ScreenPath $screenPath -Desktop $desktop -UserVisible 'not-covered'
}
if ($frame.BlueFraction -ge 0.2) {
    Report -Frame $frame -Renderer $renderer -Verdict 'frozen-on-boot-animation' -ExitCode 1 `
           -Path $path -ScreenPath $screenPath -Desktop $desktop -UserVisible 'not-covered'
}
Report -Frame $frame -Renderer $renderer -Verdict 'window-shows-something-else' -ExitCode 1 `
       -Path $path -ScreenPath $screenPath -Desktop $desktop -UserVisible 'not-covered'
