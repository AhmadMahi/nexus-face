using System.Runtime.InteropServices;
using Rafiq.Core;

namespace Rafiq.App;

/// <summary>
/// Everything that has to keep running whether or not the panel is open:
/// the poll that keeps the tray icon honest, the clipboard, the break
/// watcher, the pointer and the camera light.
/// </summary>
public sealed class Services : IDisposable
{
    readonly Device _dev;
    readonly Store _cfg;
    readonly Reminders _rem;
    readonly PointerStream _cursor = new();

    System.Windows.Forms.Timer? _slow, _fast;
    ClipboardWatcher? _clip;

    public bool AvLive { get; private set; }
    public event Action? Changed;

    double _worked;                 // seconds at the keyboard since the last break
    DateTime _lastTick = DateTime.UtcNow;
    DateTime _lastNudge = DateTime.MinValue;
    bool _lockedForIdle;

    public Services(Device dev, Store cfg, Reminders rem)
    {
        _dev = dev; _cfg = cfg; _rem = rem;
        _rem.OnDue += r => _ = _dev.Remind(r.Text);
    }

    public void Start()
    {
        _ = _dev.Refresh();

        _slow = new System.Windows.Forms.Timer { Interval = 10_000 };
        _slow.Tick += (_, __) => { _ = _dev.Refresh(); Minute(); };
        _slow.Start();

        // The pointer goes out ten times a second when it is being followed;
        // the same tick keeps the status line and reminders moving.
        _fast = new System.Windows.Forms.Timer { Interval = 100 };
        _fast.Tick += (_, __) => Fast();
        _fast.Start();

        SyncClipboard();
        SyncCursor();
        SyncAv();
    }

    int _fastCount;

    void Fast()
    {
        if (_cfg.Follow && _dev.Following) SendPointer();
        if (++_fastCount % 10 != 0) return;         // once a second from here down
        _dev.TickStatus();
        _rem.Tick();
        CheckIdle();
    }

    void SendPointer()
    {
        var p = System.Windows.Forms.Cursor.Position;
        var sc = Screen.FromPoint(p).Bounds;
        if (sc.Width <= 0 || sc.Height <= 0) return;
        int x = (int)(((p.X - sc.Left) / (double)sc.Width) * 2000 - 1000);
        // The robot looks down from the top of the monitor, so this is not
        // flipped: screen coordinates already count downwards here.
        int y = (int)(((p.Y - sc.Top) / (double)sc.Height) * 2000 - 1000);
        _cursor.Send(Math.Clamp(x, -1000, 1000), Math.Clamp(y, -1000, 1000));
    }

    // ---- breaks ----

    void Minute()
    {
        var now = DateTime.UtcNow;
        var gap = (now - _lastTick).TotalSeconds;
        _lastTick = now;

        var idle = Session.IdleSeconds();
        if (idle >= 180) { _worked = 0; }           // a real break resets it
        else _worked += Math.Min(gap, 60);          // never credit a sleeping machine

        if (!_cfg.BreakOn) return;
        if (_worked < _cfg.BreakMins * 60) return;
        if ((DateTime.UtcNow - _lastNudge).TotalSeconds < 600) return;
        _lastNudge = DateTime.UtcNow;
        _worked = 0;
        _ = _dev.Toast($"You have been at it {_cfg.BreakMins} minutes", "break", 20);
    }

    /// <summary>
    /// Away long enough that the desk should not be left open. It locks
    /// once per absence, so coming back and stepping away again is what
    /// arms it afresh.
    /// </summary>
    void CheckIdle()
    {
        if (!_cfg.LockWhenIdle) { _lockedForIdle = false; return; }
        var idle = Session.IdleSeconds();
        if (idle < 30) { _lockedForIdle = false; return; }
        if (_lockedForIdle || idle < _cfg.LockIdleMins * 60) return;
        _lockedForIdle = true;
        Session.Lock();
    }

    // ---- the rest ----

    /// <summary>
    /// Called when the break setting changes. The interval itself is read
    /// fresh every minute, so all this has to do is start the count again
    /// rather than have a new setting fire immediately off an old total.
    /// </summary>
    public void SyncBreaks()
    {
        _worked = 0;
        _lastNudge = DateTime.MinValue;
        _lastTick = DateTime.UtcNow;
    }

    public void SyncClipboard()
    {
        _clip?.Dispose(); _clip = null;
        if (!_cfg.WatchClipboard) return;
        _clip = new ClipboardWatcher(text => _ = _dev.Toast(text, "copy"));
    }

    public void SyncCursor()
    {
        _cfg.Follow = _dev.Following;
        _cfg.Save();
        if (_dev.Following && _cfg.Ip.Length > 0) _cursor.Start(_cfg.Ip);
        else _cursor.Stop();
    }

    System.Windows.Forms.Timer? _av;

    public void SyncAv()
    {
        _av?.Stop(); _av?.Dispose(); _av = null;
        if (!_cfg.WatchAv)
        {
            if (AvLive) { AvLive = false; _ = _dev.SetBusy(false, false); Changed?.Invoke(); }
            return;
        }
        bool lastCam = false, lastMic = false;
        _av = new System.Windows.Forms.Timer { Interval = 2000 };
        _av.Tick += (_, __) =>
        {
            bool c = AvWatch.CameraLive(), m = AvWatch.MicLive();
            if (c == lastCam && m == lastMic) return;
            lastCam = c; lastMic = m;
            AvLive = c || m;
            _ = _dev.SetBusy(c, m);
            Changed?.Invoke();
        };
        _av.Start();
    }

    public void Dispose()
    {
        _slow?.Dispose(); _fast?.Dispose(); _av?.Dispose();
        _clip?.Dispose(); _cursor.Dispose();
    }
}

/// <summary>
/// Mirrors what you copy, if you switch it on.
///
/// It refuses anything marked not for other eyes. Password managers set
/// these clipboard formats precisely so that tools like this one leave
/// credentials alone.
/// </summary>
public sealed class ClipboardWatcher : NativeWindow, IDisposable
{
    const int WM_CLIPBOARDUPDATE = 0x031D;
    [DllImport("user32.dll", SetLastError = true)] static extern bool AddClipboardFormatListener(IntPtr h);
    [DllImport("user32.dll", SetLastError = true)] static extern bool RemoveClipboardFormatListener(IntPtr h);

    static readonly string[] Refuse =
    {
        "ExcludeClipboardContentFromMonitorProcessing",
        "CanIncludeInClipboardHistory",
        "CanUploadToCloudClipboard",
    };

    readonly Action<string> _onCopy;

    public ClipboardWatcher(Action<string> onCopy)
    {
        _onCopy = onCopy;
        CreateHandle(new CreateParams());
        AddClipboardFormatListener(Handle);
    }

    protected override void WndProc(ref Message m)
    {
        if (m.Msg == WM_CLIPBOARDUPDATE) Read();
        base.WndProc(ref m);
    }

    void Read()
    {
        try
        {
            var data = Clipboard.GetDataObject();
            if (data == null) return;
            // Any of these present at all means the owner asked for it to be
            // left alone, whatever value it carries.
            foreach (var f in Refuse)
                if (data.GetDataPresent(f)) return;
            if (!data.GetDataPresent(DataFormats.UnicodeText)) return;
            var s = (data.GetData(DataFormats.UnicodeText) as string ?? "").Trim();
            if (s.Length == 0) return;
            _onCopy(s.Replace("\r", " ").Replace("\n", " "));
        }
        catch { /* another app can hold the clipboard open; try again next time */ }
    }

    public void Dispose()
    {
        try { RemoveClipboardFormatListener(Handle); } catch { }
        DestroyHandle();
    }
}
