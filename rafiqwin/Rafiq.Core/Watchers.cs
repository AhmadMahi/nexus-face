using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace Rafiq.Core;

/// <summary>Idle time and locking, both of which Windows answers directly.</summary>
public static class Session
{
    [StructLayout(LayoutKind.Sequential)]
    struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }

    [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LASTINPUTINFO plii);
    [DllImport("user32.dll")] static extern bool LockWorkStation();

    /// <summary>
    /// Seconds since the last keyboard or mouse event. This is one number
    /// from the system: it does not see what was typed or clicked, and it
    /// could not if it wanted to.
    /// </summary>
    public static double IdleSeconds()
    {
        var lii = new LASTINPUTINFO { cbSize = (uint)Marshal.SizeOf<LASTINPUTINFO>() };
        if (!GetLastInputInfo(ref lii)) return 0;
        // Both are 32 bit millisecond counters that wrap about every 49 days,
        // so the subtraction is done in that width on purpose and only then
        // widened. Doing it any other way reads as weeks of idleness once.
        uint now = (uint)Environment.TickCount;
        return unchecked(now - lii.dwTime) / 1000.0;
    }

    /// <summary>Locks the desktop. A plain, documented Windows call.</summary>
    public static bool Lock() { try { return LockWorkStation(); } catch { return false; } }
}

/// <summary>
/// Is anything using the camera or the microphone?
///
/// Windows records this itself, per application, under the privacy settings
/// it already shows you. An app that is using a device has a start time and
/// no stop time. Nothing is heard, nothing is seen and nothing is recorded
/// here: the only thing this reads is whether a device is switched on.
///
/// One honest limit. This reflects apps that go through the Windows privacy
/// framework, which is almost everything modern. Something reaching the
/// hardware the old way, around that framework, would not appear.
/// </summary>
public static class AvWatch
{
    const string Base = @"SOFTWARE\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore";

    public static bool CameraLive() => AnyInUse("webcam");
    public static bool MicLive()    => AnyInUse("microphone");

    static bool AnyInUse(string kind)
    {
        foreach (var root in new[] { Registry.CurrentUser, Registry.LocalMachine })
        {
            try
            {
                using var k = root.OpenSubKey($@"{Base}\{kind}");
                if (k == null) continue;
                if (BranchInUse(k)) return true;
            }
            catch { }
        }
        return false;
    }

    static bool BranchInUse(RegistryKey k)
    {
        foreach (var name in k.GetSubKeyNames())
        {
            try
            {
                using var app = k.OpenSubKey(name);
                if (app == null) continue;
                // Desktop programs are gathered under one key of their own.
                if (name.Equals("NonPackaged", StringComparison.OrdinalIgnoreCase))
                {
                    if (BranchInUse(app)) return true;
                    continue;
                }
                if (InUse(app)) return true;
            }
            catch { }
        }
        return false;
    }

    /// <summary>A start with no stop after it means it is live right now.</summary>
    static bool InUse(RegistryKey app)
    {
        var start = app.GetValue("LastUsedTimeStart");
        var stop  = app.GetValue("LastUsedTimeStop");
        if (start == null || stop == null) return false;
        long s = Convert.ToInt64(start), e = Convert.ToInt64(stop);
        return s > 0 && e == 0;
    }
}

/// <summary>
/// Which of the system's two looks to draw in, when following the system.
/// </summary>
public static class Theme
{
    public static bool SystemIsLight()
    {
        try
        {
            using var k = Registry.CurrentUser.OpenSubKey(
                @"SOFTWARE\Microsoft\Windows\CurrentVersion\Themes\Personalize");
            return Convert.ToInt32(k?.GetValue("AppsUseLightTheme") ?? 1) != 0;
        }
        catch { return true; }
    }
}
