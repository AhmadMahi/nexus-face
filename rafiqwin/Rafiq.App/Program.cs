using Rafiq.Core;

namespace Rafiq.App;

static class Program
{
    /// <summary>
    /// A tray app has nowhere to show a crash, so anything that escapes is
    /// written down instead of disappearing. This is also what lets the
    /// build machine say why it fell over.
    /// </summary>
    static void LogCrash(object? e)
    {
        try
        {
            Directory.CreateDirectory(Store.Dir);
            File.AppendAllText(Path.Combine(Store.Dir, "crash.log"),
                $"{DateTime.Now:u}  {e}{Environment.NewLine}{Environment.NewLine}");
        }
        catch { }
        try { Console.Error.WriteLine(e); } catch { }
    }

    [STAThread]
    static int Main(string[] args)
    {
        // Given arguments, this is the command rather than the tray app.
        if (args.Length > 0) return Cli.Run(args).GetAwaiter().GetResult();

        AppDomain.CurrentDomain.UnhandledException += (_, a) => LogCrash(a.ExceptionObject);
        Application.ThreadException += (_, a) => LogCrash(a.Exception);
        Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);

        // One Rafiq at a time, or two tray icons fight over the same robot.
        using var only = new Mutex(true, "Local\\RafiqMenuBarApp", out bool first);
        if (!first) return 0;

        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);

        try
        {
            var cfg = Store.Load();
            var dev = new Device(cfg);
            var rem = new Reminders();
            using var svc = new Services(dev, cfg, rem);
            using var tray = new TrayHost(dev, cfg, rem, svc);
            svc.Start();
            Application.Run();
        }
        catch (Exception e)
        {
            LogCrash(e);
            throw;
        }
        return 0;
    }
}

/// <summary>
/// The tray icon and the window it opens. The icon's eyes are the state,
/// so it has to be right whether or not the panel has ever been opened.
/// </summary>
sealed class TrayHost : IDisposable
{
    readonly NotifyIcon _icon;
    readonly PanelForm _panel;
    readonly Device _dev;
    readonly Store _cfg;
    Icon? _current;
    RafiqIcon.Face _face = (RafiqIcon.Face)(-1);
    bool _lastLight;

    public TrayHost(Device dev, Store cfg, Reminders rem, Services svc)
    {
        _dev = dev; _cfg = cfg;
        _panel = new PanelForm(dev, cfg, rem, svc);

        var menu = new ContextMenuStrip();
        menu.Items.Add("Open", null, (_, __) => Open());
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("Quit", null, (_, __) => Application.Exit());

        _icon = new NotifyIcon
        {
            Text = "Rafiq",
            Visible = true,
            ContextMenuStrip = menu,
            Icon = Draw()
        };
        _icon.MouseClick += (_, e) => { if (e.Button == MouseButtons.Left) Open(); };

        dev.Changed += () =>
        {
            if (_icon.Container == null && !_icon.Visible) return;
            try { _icon.Icon = Draw(); } catch { }
        };
        svc.Changed += () => { try { _icon.Icon = Draw(); } catch { } };
    }

    void Open()
    {
        if (_panel.Visible) { _panel.Hide(); return; }
        _panel.ShowNear();
    }

    /// <summary>
    /// Red means it answered before and has stopped. Before the first reply
    /// there is nothing to report, so it stays grey rather than claiming a
    /// fault that has not happened.
    /// </summary>
    Icon Draw()
    {
        var face = _cfg.Ip.Length == 0 ? RafiqIcon.Face.Unset
                 : _dev.Reachable == true ? RafiqIcon.Face.Linked
                 : _dev.Reachable == false ? RafiqIcon.Face.Adrift
                 : RafiqIcon.Face.Unset;
        bool light = Theme.SystemIsLight();
        if (_current != null && face == _face && light == _lastLight) return _current;

        _face = face; _lastLight = light;
        var old = _current;
        // A light taskbar wants a dark head drawn on it, and the other way
        // round, which is the opposite of what the panel does.
        _current = RafiqIcon.Make(face, lightTaskbar: light, px: 32);
        old?.Dispose();
        return _current;
    }

    public void Dispose()
    {
        _icon.Visible = false;
        _icon.Dispose();
        _current?.Dispose();
        _panel.Dispose();
    }
}
