using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;
using Rafiq.Core;

namespace Rafiq.App;

/// <summary>
/// The window the tray icon opens. Borderless and rounded, with the
/// system's own backdrop behind it on Windows 11.
/// </summary>
public sealed partial class PanelForm : Form
{
    readonly Device _dev;
    readonly Store _cfg;
    readonly Reminders _rem;
    readonly Services _svc;
    readonly Updater _up = new();

    enum View { Grid, Settings, Pair, Focus, Break, Remind, Phrases }
    View _view = View.Grid;

    Skin _skin = Skin.For(true);
    readonly List<TileButton> _tiles = new();
    TextBox _compose = null!, _pairBox = null!;
    Panel _body = null!;

    public PanelForm(Device dev, Store cfg, Reminders rem, Services svc)
    {
        _dev = dev; _cfg = cfg; _rem = rem; _svc = svc;

        FormBorderStyle = FormBorderStyle.None;
        ShowInTaskbar = false;
        StartPosition = FormStartPosition.Manual;
        KeyPreview = true;
        Width = Scale(330);
        Height = Scale(470);
        SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint, true);

        _dev.Changed += OnChanged;
        Build();
        ApplyTheme();
    }

    int Scale(int v) => (int)(v * DeviceDpi / 96f);

    // ---------------------------------------------------------------

    void OnChanged()
    {
        if (IsDisposed) return;
        if (InvokeRequired) { BeginInvoke(new Action(OnChanged)); return; }
        Refill();
    }

    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);
        ApplyTheme();
        Refill();
        RoundAndBlur();
    }

    protected override void OnDeactivate(EventArgs e)
    {
        base.OnDeactivate(e);
        Hide();                           // click away and it goes, like a menu
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (e.KeyCode == Keys.Escape)
        {
            if (_view == View.Grid) Hide(); else { _view = View.Grid; Build(); Refill(); }
            e.Handled = true;
        }
        base.OnKeyDown(e);
    }

    public void ShowNear()
    {
        var wa = Screen.PrimaryScreen!.WorkingArea;
        var mouse = Cursor.Position;
        var sc = Screen.FromPoint(mouse);
        wa = sc.WorkingArea;
        Left = Math.Max(wa.Left + 8, Math.Min(mouse.X - Width / 2, wa.Right - Width - 8));
        Top  = wa.Bottom - Height - 8;    // the taskbar is nearly always at the foot
        if (sc.Bounds.Top == wa.Top && wa.Top > sc.Bounds.Top) Top = wa.Top + 8;
        Show();
        Activate();
        BringToFront();
    }

    // ---------------------------------------------------------------
    //  chrome
    // ---------------------------------------------------------------

    [DllImport("dwmapi.dll")]
    static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int val, int size);

    /// <summary>
    /// Rounded corners and the system backdrop, both of which Windows 11
    /// does for us. On Windows 10 these calls simply fail and it stays a
    /// plain rectangle, which is the right thing to look like there.
    /// </summary>
    void RoundAndBlur()
    {
        try
        {
            int round = 2;                                   // DWMWCP_ROUND
            DwmSetWindowAttribute(Handle, 33, ref round, sizeof(int));
            int dark = _skin.Light ? 0 : 1;
            DwmSetWindowAttribute(Handle, 20, ref dark, sizeof(int));   // dark title chrome
            int backdrop = 3;                                // DWMSBT_TRANSIENTWINDOW, the menu look
            DwmSetWindowAttribute(Handle, 38, ref backdrop, sizeof(int));
        }
        catch { }
    }

    void ApplyTheme()
    {
        bool light = _cfg.Theme switch
        {
            "light" => true,
            "dark"  => false,
            _       => Theme.SystemIsLight()
        };
        _skin = Skin.For(light);
        BackColor = _skin.Bg;
        foreach (var t in _tiles) t.Skin = _skin;
        Paint -= PaintChrome;
        Paint += PaintChrome;
        Invalidate(true);
    }

    void PaintChrome(object? s, PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;

        int pad = Scale(13);
        using var title = new Font("Segoe UI Semibold", Scale(11), FontStyle.Regular, GraphicsUnit.Pixel);
        using var tb = new SolidBrush(_skin.Text);
        using var db = new SolidBrush(_skin.Dim);

        var dot = _cfg.Ip.Length == 0 ? _skin.Dim
                : _dev.Reachable == true ? (_dev.Paired && !_dev.Linked ? Color.Orange : Color.FromArgb(52, 199, 89))
                : _dev.Reachable == false ? Color.FromArgb(235, 72, 62) : _skin.Dim;
        using (var b = new SolidBrush(dot)) g.FillEllipse(b, pad, pad + Scale(4), Scale(8), Scale(8));
        g.DrawString("R A F I Q", title, tb, pad + Scale(14), pad + Scale(1));

        if (_dev.FocusLeft > 0)
        {
            var s2 = $"{_dev.FocusLeft / 60 + 1}m";
            using var f = new Font("Segoe UI", Scale(10), FontStyle.Regular, GraphicsUnit.Pixel);
            var w = g.MeasureString(s2, f).Width;
            var r = new RectangleF(pad + Scale(78), pad, w + Scale(10), Scale(17));
            using (var p = RafiqIcon.Rounded(r, Scale(8)))
            using (var b = new SolidBrush(_skin.CardOn)) g.FillPath(b, p);
            using var ab = new SolidBrush(_skin.Accent);
            g.DrawString(s2, f, ab, r.X + Scale(5), r.Y + Scale(2));
        }

        if (_dev.Status.Length > 0)
        {
            using var f = new Font("Segoe UI", Scale(10), FontStyle.Regular, GraphicsUnit.Pixel);
            g.DrawString(_dev.Status, f, db, pad, Height - Scale(16));
        }
    }

    // ---------------------------------------------------------------
    //  layout
    // ---------------------------------------------------------------

    void Build()
    {
        SuspendLayout();
        Controls.Clear();
        _tiles.Clear();

        int pad = Scale(13), top = Scale(40);

        // the gear and the power button, top right
        AddGlyphButton("", Width - Scale(52), pad, () => { _view = _view == View.Settings ? View.Grid : View.Settings; Build(); Refill(); });
        AddGlyphButton("", Width - Scale(30), pad, () => { Application.Exit(); });

        _body = new Panel { Left = pad, Top = top, Width = Width - pad * 2,
                            Height = Height - top - Scale(20), BackColor = Color.Transparent };
        Controls.Add(_body);

        switch (_view)
        {
            case View.Grid:     BuildGrid();     break;
            case View.Settings: BuildSettings(); break;
            case View.Pair:     BuildPair();     break;
            case View.Focus:    BuildMinutes("Focus for", new[]{5,10,15,25,30,45,60,90},
                                    "The panel shows the countdown, then rests, then shows it again. It will not drop off until the time is up.",
                                    m => Fire(async () => await _dev.StartFocus(m))); break;
            case View.Break:    BuildMinutes("On a break for", new[]{5,10,15,20,30,45,60,90},
                                    "The robot holds the sign and this machine locks straight away. It comes back when the time is up.",
                                    m => Fire(async () => { await _dev.StartBreak(m); await Task.Delay(400); Session.Lock(); })); break;
            case View.Remind:   BuildRemind();   break;
            case View.Phrases:  BuildPhrases();  break;
        }
        ResumeLayout();
        ApplyTheme();
    }

    void AddGlyphButton(string glyph, int x, int y, Action act)
    {
        var b = new Label {
            Text = glyph, Left = x, Top = y, Width = Scale(20), Height = Scale(20),
            Font = new Font("Segoe MDL2 Assets", Scale(12), FontStyle.Regular, GraphicsUnit.Pixel),
            ForeColor = _skin.Dim, BackColor = Color.Transparent,
            TextAlign = ContentAlignment.MiddleCenter, Cursor = Cursors.Hand };
        b.Click += (_, __) => act();
        Controls.Add(b);
        b.BringToFront();
    }

    void BuildGrid()
    {
        int gap = Scale(7);
        int cw = (_body.Width - gap * 2) / 3, ch = Scale(66);

        void Tile(int col, int row, string glyph, string title, string detail,
                  bool on, bool available, Action act)
        {
            var t = new TileButton {
                Left = col * (cw + gap), Top = row * (ch + gap), Width = cw, Height = ch,
                Glyph = glyph, Title = title, Detail = detail, On = on, Available = available,
                Skin = _skin };
            if (available) t.Click += (_, __) => act();
            _body.Controls.Add(t);
            _tiles.Add(t);
        }

        var pend = _rem.Pending.Count;

        // row one
        Tile(0, 0, "", "Phrases", "saved lines", false, true, () => { _view = View.Phrases; Build(); Refill(); });
        Tile(1, 0, "", "Focus", _dev.FocusLeft > 0 ? $"{_dev.FocusLeft / 60 + 1} min left" : "",
             _dev.FocusLeft > 0, true,
             () => { if (_dev.FocusLeft > 0) Fire(_dev.StopFocus); else { _view = View.Focus; Build(); Refill(); } });
        Tile(2, 0, "", "Follow", "the pointer", _dev.Following, true,
             () => Fire(async () => { await _dev.SetFollow(!_dev.Following); _svc.SyncCursor(); }));

        // row two
        Tile(0, 1, "", "Relax", "screensaver", _dev.Relaxing, true,
             () => Fire(async () => await _dev.SetRelax(!_dev.Relaxing)));
        Tile(1, 1, "", "Clipboard", _cfg.WatchClipboard ? "mirroring" : "off",
             _cfg.WatchClipboard, true,
             () => { _cfg.WatchClipboard = !_cfg.WatchClipboard; _cfg.Save(); _svc.SyncClipboard(); Refill(); });
        Tile(2, 1, "", "Breaks", _cfg.BreakOn ? $"every {_cfg.BreakMins}m" : "off",
             _cfg.BreakOn, true,
             () => { _cfg.BreakOn = !_cfg.BreakOn; _cfg.Save(); _svc.SyncBreaks(); Refill(); });

        // row three
        Tile(0, 2, "", "Remind me", pend == 0 ? "nothing set" : $"{pend} waiting", pend > 0, true,
             () => { _view = View.Remind; Build(); Refill(); });
        Tile(1, 2, "", "On a break",
             _dev.DndLeft > 0 ? $"{_dev.DndLeft / 60 + 1} min left" : "locks this PC",
             _dev.DndLeft > 0, true,
             () => { if (_dev.DndLeft > 0) Fire(_dev.EndBreak); else { _view = View.Break; Build(); Refill(); } });
        Tile(2, 2, "", "Camera & mic",
             _cfg.WatchAv ? (_svc.AvLive ? "live now" : "watching") : "off", _cfg.WatchAv, true,
             () => { _cfg.WatchAv = !_cfg.WatchAv; _cfg.Save(); _svc.SyncAv(); Refill(); });

        // row four
        Tile(0, 3, "", "Update", "the robot", false, true, () => Fire(_dev.CheckUpdate));
        Tile(1, 3, "", "Deep sleep", "power to wake", false, true, () => Fire(_dev.DeepSleep));
        // Wired up and tested, but deliberately inert for now.
        Tile(2, 3, "", "Draw", "not yet", false, false, () => { });

        // the compose field, below the grid
        int y = 4 * (ch + gap) + Scale(4);
        _compose = new TextBox {
            Left = 0, Top = y, Width = _body.Width - Scale(34), BorderStyle = BorderStyle.FixedSingle,
            Font = new Font("Segoe UI", Scale(12), FontStyle.Regular, GraphicsUnit.Pixel),
            PlaceholderText = "Say something" };
        _compose.KeyDown += (_, e) =>
        {
            if (e.KeyCode != Keys.Enter) return;
            e.SuppressKeyPress = true;
            var t = _compose.Text; _compose.Text = "";
            Fire(async () => await _dev.Say(t));
        };
        _body.Controls.Add(_compose);

        var send = new Label {
            Text = "", Left = _body.Width - Scale(26), Top = y, Width = Scale(24), Height = Scale(24),
            Font = new Font("Segoe MDL2 Assets", Scale(14), FontStyle.Regular, GraphicsUnit.Pixel),
            TextAlign = ContentAlignment.MiddleCenter, Cursor = Cursors.Hand, BackColor = Color.Transparent };
        send.Click += (_, __) => { var t = _compose.Text; _compose.Text = ""; Fire(async () => await _dev.Say(t)); };
        _body.Controls.Add(send);
    }

    void Fire(Func<Task> f) => _ = Task.Run(async () => { try { await f(); } catch { } });

    void Refill()
    {
        if (_view == View.Grid && _tiles.Count == 12)
        {
            var pend = _rem.Pending.Count;
            _tiles[1].Detail = _dev.FocusLeft > 0 ? $"{_dev.FocusLeft / 60 + 1} min left" : "";
            _tiles[1].On = _dev.FocusLeft > 0;
            _tiles[2].On = _dev.Following;
            _tiles[3].On = _dev.Relaxing;
            _tiles[4].Detail = _cfg.WatchClipboard ? "mirroring" : "off"; _tiles[4].On = _cfg.WatchClipboard;
            _tiles[5].Detail = _cfg.BreakOn ? $"every {_cfg.BreakMins}m" : "off"; _tiles[5].On = _cfg.BreakOn;
            _tiles[6].Detail = pend == 0 ? "nothing set" : $"{pend} waiting"; _tiles[6].On = pend > 0;
            _tiles[7].Detail = _dev.DndLeft > 0 ? $"{_dev.DndLeft / 60 + 1} min left" : "locks this PC";
            _tiles[7].On = _dev.DndLeft > 0;
            _tiles[8].Detail = _cfg.WatchAv ? (_svc.AvLive ? "live now" : "watching") : "off";
            _tiles[8].On = _cfg.WatchAv;
            foreach (var t in _tiles) t.Invalidate();
        }
        if (_dev.Pairing && _view != View.Pair) { _view = View.Pair; Build(); }
        Invalidate();
    }
}
