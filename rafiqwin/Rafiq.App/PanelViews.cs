using Rafiq.Core;

namespace Rafiq.App;

/// <summary>
/// The screens a tile opens. Each replaces the grid rather than sitting
/// over it, so one thing is on screen at a time.
/// </summary>
public sealed partial class PanelForm
{
    Label Head(string text, int y)
    {
        var l = new Label {
            Text = text, Left = 0, Top = y, Width = _body.Width - Scale(24), Height = Scale(18),
            Font = new Font("Segoe UI Semibold", Scale(12), FontStyle.Regular, GraphicsUnit.Pixel),
            ForeColor = _skin.Text, BackColor = Color.Transparent };
        _body.Controls.Add(l);

        var x = new Label {
            Text = "", Left = _body.Width - Scale(20), Top = y, Width = Scale(20), Height = Scale(18),
            Font = new Font("Segoe MDL2 Assets", Scale(12), FontStyle.Regular, GraphicsUnit.Pixel),
            ForeColor = _skin.Dim, BackColor = Color.Transparent, Cursor = Cursors.Hand,
            TextAlign = ContentAlignment.MiddleCenter };
        x.Click += (_, __) => { _view = View.Grid; _dev.Pairing = false; Build(); Refill(); };
        _body.Controls.Add(x);
        return l;
    }

    Label Note(string text, int y)
    {
        var l = new Label {
            Text = text, Left = 0, Top = y, Width = _body.Width, Height = Scale(34),
            Font = new Font("Segoe UI", Scale(10), FontStyle.Regular, GraphicsUnit.Pixel),
            ForeColor = _skin.Dim, BackColor = Color.Transparent };
        _body.Controls.Add(l);
        return l;
    }

    Button Chip(string text, int x, int y, int w, int h, Action act, bool strong = false)
    {
        var b = new Button {
            Text = text, Left = x, Top = y, Width = w, Height = h, FlatStyle = FlatStyle.Flat,
            Font = new Font("Segoe UI", Scale(12), FontStyle.Regular, GraphicsUnit.Pixel),
            BackColor = strong ? _skin.CardOn : _skin.Card,
            ForeColor = strong ? _skin.Accent : _skin.Text, Cursor = Cursors.Hand };
        b.FlatAppearance.BorderSize = 0;
        b.Click += (_, __) => act();
        _body.Controls.Add(b);
        return b;
    }

    // ---------------------------------------------------------------

    void BuildMinutes(string title, int[] choices, string note, Action<int> pick)
    {
        Head(title, 0);
        int gap = Scale(6), cw = (_body.Width - gap * 3) / 4, ch = Scale(32);
        for (int i = 0; i < choices.Length; i++)
        {
            int m = choices[i];
            Chip(m >= 60 && m % 60 == 0 ? $"{m / 60}h" : m.ToString(),
                 (i % 4) * (cw + gap), Scale(26) + (i / 4) * (ch + gap), cw, ch,
                 () => { _view = View.Grid; Build(); Refill(); pick(m); });
        }
        Note(note, Scale(26) + 2 * (ch + gap) + Scale(6));
    }

    void BuildPhrases()
    {
        Head("Quick phrases", 0);
        int y = Scale(26);
        foreach (var p in _cfg.Phrases.Take(8))
        {
            var text = p;
            Chip(text, 0, y, _body.Width, Scale(30), () =>
            {
                _view = View.Grid; Build(); Refill();
                Fire(async () => await _dev.Say(text));
            });
            y += Scale(35);
        }
        if (!_cfg.Phrases.Any()) Note("Add some in settings, one per line.", y);
    }

    void BuildPair()
    {
        Head("Pair with the robot", 0);
        Note("Six digits are on its panel now. They last three minutes.", Scale(24));
        _pairBox = new TextBox {
            Left = 0, Top = Scale(58), Width = _body.Width, BorderStyle = BorderStyle.FixedSingle,
            Font = new Font("Consolas", Scale(22), FontStyle.Regular, GraphicsUnit.Pixel),
            TextAlign = HorizontalAlignment.Center, MaxLength = 6, PlaceholderText = "000000" };
        _pairBox.TextChanged += (_, __) =>
        {
            var d = new string(_pairBox.Text.Where(char.IsDigit).ToArray());
            if (d != _pairBox.Text) { _pairBox.Text = d; _pairBox.SelectionStart = d.Length; return; }
            if (d.Length == 6) Fire(async () => await _dev.Pair(d));
        };
        _body.Controls.Add(_pairBox);
        _pairBox.Focus();

        if (_dev.PairError.Length > 0)
        {
            var e = Note(_dev.PairError, Scale(96));
            e.ForeColor = Color.FromArgb(220, 70, 60);
        }
        Chip("New code", 0, Scale(120), Scale(90), Scale(28), () => Fire(_dev.RequestCode));
    }

    void BuildRemind()
    {
        Head("Remind me", 0);
        var text = new TextBox {
            Left = 0, Top = Scale(26), Width = _body.Width, BorderStyle = BorderStyle.FixedSingle,
            Font = new Font("Segoe UI", Scale(12), FontStyle.Regular, GraphicsUnit.Pixel),
            PlaceholderText = "What about?" };
        _body.Controls.Add(text);

        // Either in a while, or at a time. Two plain menus for the clock
        // rather than a date control: they always take a click, and they
        // are quicker than typing a time anyway.
        int chosen = 15;
        bool atTime = false;
        var chips = new List<Button>();
        int[] quick = { 5, 10, 15, 30, 45, 60, 90, 120 };
        int gap = Scale(5), cw = (_body.Width - gap * 3) / 4, ch = Scale(26);

        var mode = new ComboBox {
            Left = 0, Top = Scale(56), Width = Scale(70), DropDownStyle = ComboBoxStyle.DropDownList,
            Font = new Font("Segoe UI", Scale(11), FontStyle.Regular, GraphicsUnit.Pixel) };
        mode.Items.AddRange(new object[] { "In", "At" });
        mode.SelectedIndex = 0;
        _body.Controls.Add(mode);

        var hour = new ComboBox {
            Left = Scale(78), Top = Scale(56), Width = Scale(64), DropDownStyle = ComboBoxStyle.DropDownList,
            Font = new Font("Segoe UI", Scale(11), FontStyle.Regular, GraphicsUnit.Pixel), Visible = false };
        for (int h = 0; h < 24; h++) hour.Items.Add(h.ToString("00"));
        hour.SelectedIndex = DateTime.Now.AddHours(1).Hour;
        var minute = new ComboBox {
            Left = Scale(148), Top = Scale(56), Width = Scale(64), DropDownStyle = ComboBoxStyle.DropDownList,
            Font = new Font("Segoe UI", Scale(11), FontStyle.Regular, GraphicsUnit.Pixel), Visible = false };
        for (int m = 0; m < 60; m += 5) minute.Items.Add(m.ToString("00"));
        minute.SelectedIndex = 0;
        var dayNote = new Label {
            Left = Scale(218), Top = Scale(59), Width = Scale(80), Height = Scale(18),
            Font = new Font("Segoe UI", Scale(10), FontStyle.Regular, GraphicsUnit.Pixel),
            ForeColor = _skin.Dim, BackColor = Color.Transparent, Visible = false };
        _body.Controls.Add(hour); _body.Controls.Add(minute); _body.Controls.Add(dayNote);

        // The next time the clock shows this. One already gone means
        // tomorrow, which is what anyone setting 7am at midnight expects.
        DateTime NextAt()
        {
            var now = DateTime.Now;
            var t = new DateTime(now.Year, now.Month, now.Day,
                                 hour.SelectedIndex, minute.SelectedIndex * 5, 0);
            return t <= now ? t.AddDays(1) : t;
        }
        void Day() => dayNote.Text = NextAt().Date == DateTime.Today ? "today" : "tomorrow";
        hour.SelectedIndexChanged += (_, __) => Day();
        minute.SelectedIndexChanged += (_, __) => Day();
        Day();

        for (int i = 0; i < quick.Length; i++)
        {
            int m = quick[i];
            var b = Chip(m >= 60 && m % 60 == 0 ? $"{m / 60}h" : $"{m}m",
                         (i % 4) * (cw + gap), Scale(88) + (i / 4) * (ch + gap), cw, ch,
                         () => { chosen = m;
                                 foreach (var c in chips) { c.BackColor = _skin.Card; c.ForeColor = _skin.Text; }
                                 var me = chips[Array.IndexOf(quick, m)];
                                 me.BackColor = _skin.CardOn; me.ForeColor = _skin.Accent; });
            chips.Add(b);
        }
        chips[2].BackColor = _skin.CardOn; chips[2].ForeColor = _skin.Accent;

        mode.SelectedIndexChanged += (_, __) =>
        {
            atTime = mode.SelectedIndex == 1;
            hour.Visible = minute.Visible = dayNote.Visible = atTime;
            foreach (var c in chips) c.Visible = !atTime;
        };

        int y = Scale(88) + 2 * (ch + gap) + Scale(6);
        void Add()
        {
            var t = text.Text.Trim();
            if (t.Length == 0) return;
            if (atTime) _rem.Add(t, NextAt());
            else        _rem.AddInMinutes(t, chosen);
            text.Text = "";
            _dev.Flash("Reminder set");
            Build(); Refill();
        }
        text.KeyDown += (_, e) => { if (e.KeyCode == Keys.Enter) { e.SuppressKeyPress = true; Add(); } };
        Chip("Add reminder", 0, y, _body.Width, Scale(30), Add, strong: true);

        y += Scale(38);
        foreach (var r in _rem.Pending.Take(5))
        {
            var when = r.FireAt.Date == DateTime.Today ? r.FireAt.ToString("HH:mm")
                                                       : r.FireAt.ToString("ddd HH:mm");
            var row = new Label {
                Text = $"{when}   {r.Text}", Left = 0, Top = y, Width = _body.Width - Scale(22),
                Height = Scale(18),
                Font = new Font("Segoe UI", Scale(11), FontStyle.Regular, GraphicsUnit.Pixel),
                ForeColor = _skin.Text, BackColor = Color.Transparent };
            _body.Controls.Add(row);
            var rr = r;
            var del = new Label {
                Text = "", Left = _body.Width - Scale(18), Top = y, Width = Scale(18),
                Height = Scale(18),
                Font = new Font("Segoe MDL2 Assets", Scale(10), FontStyle.Regular, GraphicsUnit.Pixel),
                ForeColor = _skin.Dim, BackColor = Color.Transparent, Cursor = Cursors.Hand };
            del.Click += (_, __) => { _rem.Remove(rr); Build(); Refill(); };
            _body.Controls.Add(del);
            y += Scale(22);
        }
    }

    void BuildSettings()
    {
        Head("Settings", 0);
        int y = Scale(28);

        var addr = new TextBox {
            Left = 0, Top = y, Width = _body.Width - Scale(60), BorderStyle = BorderStyle.FixedSingle,
            Font = new Font("Consolas", Scale(12), FontStyle.Regular, GraphicsUnit.Pixel),
            Text = _cfg.Ip, PlaceholderText = "192.168.1.42" };
        _body.Controls.Add(addr);
        void Save()
        {
            _cfg.Ip = addr.Text.Trim(); _cfg.Save();
            Fire(_dev.Refresh);
            if (_cfg.Ip.Length > 0) { _view = View.Grid; Build(); Refill(); }
        }
        addr.KeyDown += (_, e) => { if (e.KeyCode == Keys.Enter) { e.SuppressKeyPress = true; Save(); } };
        Chip("Save", _body.Width - Scale(54), y, Scale(54), Scale(24), Save);
        y += Scale(28);
        Note("On the robot: SYSTEM shows it.", y);
        y += Scale(22);

        // pairing
        var pairLabel = new Label {
            Text = _dev.Paired ? "Paired: only this PC can drive it"
                               : "Not paired: anyone on your network can",
            Left = 0, Top = y, Width = _body.Width - Scale(70), Height = Scale(18),
            Font = new Font("Segoe UI", Scale(11), FontStyle.Regular, GraphicsUnit.Pixel),
            ForeColor = _skin.Text, BackColor = Color.Transparent };
        _body.Controls.Add(pairLabel);
        Chip(_dev.Paired ? "Forget" : "Pair", _body.Width - Scale(64), y - Scale(3), Scale(64), Scale(24),
             () => { if (_dev.Paired) Fire(_dev.Unpair); else { _view = View.Grid; Build(); Fire(_dev.RequestCode); } });
        y += Scale(30);

        // break interval, with a custom setting
        _body.Controls.Add(new Label {
            Text = "Break every", Left = 0, Top = y + Scale(3), Width = Scale(80), Height = Scale(18),
            Font = new Font("Segoe UI", Scale(11), FontStyle.Regular, GraphicsUnit.Pixel),
            ForeColor = _skin.Text, BackColor = Color.Transparent });
        var breaks = new ComboBox {
            Left = Scale(86), Top = y, Width = _body.Width - Scale(86), DropDownStyle = ComboBoxStyle.DropDownList,
            Font = new Font("Segoe UI", Scale(11), FontStyle.Regular, GraphicsUnit.Pixel) };
        int[] choices = { 5, 10, 20, 30, 45, 60, 90 };
        foreach (var c in choices) breaks.Items.Add($"{c} min");
        breaks.Items.Add("Custom");
        int idx = Array.IndexOf(choices, _cfg.BreakMins);
        breaks.SelectedIndex = idx >= 0 ? idx : choices.Length;
        _body.Controls.Add(breaks);
        y += Scale(28);

        var custom = new NumericUpDown {
            Left = Scale(86), Top = y, Width = Scale(80), Minimum = 5, Maximum = 90, Increment = 5,
            Value = Math.Clamp(_cfg.BreakMins, 5, 90),
            Font = new Font("Segoe UI", Scale(11), FontStyle.Regular, GraphicsUnit.Pixel),
            Visible = breaks.SelectedIndex == choices.Length };
        var customNote = new Label {
            Text = "5 to 90 minutes", Left = Scale(172), Top = y + Scale(3), Width = Scale(110), Height = Scale(18),
            Font = new Font("Segoe UI", Scale(10), FontStyle.Regular, GraphicsUnit.Pixel),
            ForeColor = _skin.Dim, BackColor = Color.Transparent, Visible = custom.Visible };
        custom.ValueChanged += (_, __) => { _cfg.BreakMins = (int)custom.Value; _cfg.BreakCustom = _cfg.BreakMins; _cfg.Save(); _svc.SyncBreaks(); };
        breaks.SelectedIndexChanged += (_, __) =>
        {
            bool isCustom = breaks.SelectedIndex == choices.Length;
            custom.Visible = customNote.Visible = isCustom;
            _cfg.BreakMins = isCustom ? Math.Max(5, _cfg.BreakCustom) : choices[breaks.SelectedIndex];
            if (isCustom) custom.Value = Math.Clamp(_cfg.BreakMins, 5, 90);
            _cfg.Save(); _svc.SyncBreaks();
        };
        _body.Controls.Add(custom); _body.Controls.Add(customNote);
        y += Scale(30);

        // locking when you walk away
        var lockBox = new CheckBox {
            Text = "Lock when I walk away", Left = 0, Top = y, Width = _body.Width - Scale(80),
            Height = Scale(20), Checked = _cfg.LockWhenIdle,
            Font = new Font("Segoe UI", Scale(11), FontStyle.Regular, GraphicsUnit.Pixel),
            ForeColor = _skin.Text, BackColor = Color.Transparent };
        var lockAfter = new ComboBox {
            Left = _body.Width - Scale(76), Top = y - Scale(2), Width = Scale(76),
            DropDownStyle = ComboBoxStyle.DropDownList,
            Font = new Font("Segoe UI", Scale(11), FontStyle.Regular, GraphicsUnit.Pixel),
            Visible = _cfg.LockWhenIdle };
        int[] mins = { 2, 5, 10, 15, 30 };
        foreach (var m in mins) lockAfter.Items.Add($"{m} min");
        lockAfter.SelectedIndex = Math.Max(0, Array.IndexOf(mins, _cfg.LockIdleMins));
        lockBox.CheckedChanged += (_, __) => { _cfg.LockWhenIdle = lockBox.Checked; lockAfter.Visible = lockBox.Checked; _cfg.Save(); };
        lockAfter.SelectedIndexChanged += (_, __) => { _cfg.LockIdleMins = mins[lockAfter.SelectedIndex]; _cfg.Save(); };
        _body.Controls.Add(lockBox); _body.Controls.Add(lockAfter);
        y += Scale(26);

        var clip = new CheckBox {
            Text = "Send what I copy", Left = 0, Top = y, Width = _body.Width, Height = Scale(20),
            Checked = _cfg.WatchClipboard,
            Font = new Font("Segoe UI", Scale(11), FontStyle.Regular, GraphicsUnit.Pixel),
            ForeColor = _skin.Text, BackColor = Color.Transparent };
        clip.CheckedChanged += (_, __) => { _cfg.WatchClipboard = clip.Checked; _cfg.Save(); _svc.SyncClipboard(); };
        _body.Controls.Add(clip);
        y += Scale(22);
        Note("Skips anything a password manager marks.", y);
        y += Scale(20);

        // phrases
        var ph = new TextBox {
            Left = 0, Top = y, Width = _body.Width, Height = Scale(54), Multiline = true,
            ScrollBars = ScrollBars.Vertical, BorderStyle = BorderStyle.FixedSingle,
            Font = new Font("Segoe UI", Scale(11), FontStyle.Regular, GraphicsUnit.Pixel),
            Text = _cfg.PhrasesRaw.Replace("\n", Environment.NewLine) };
        ph.Leave += (_, __) => { _cfg.PhrasesRaw = ph.Text.Replace(Environment.NewLine, "\n"); _cfg.Save(); };
        _body.Controls.Add(ph);
        y += Scale(60);

        // appearance
        _body.Controls.Add(new Label {
            Text = "Appearance", Left = 0, Top = y + Scale(3), Width = Scale(80), Height = Scale(18),
            Font = new Font("Segoe UI", Scale(11), FontStyle.Regular, GraphicsUnit.Pixel),
            ForeColor = _skin.Text, BackColor = Color.Transparent });
        var theme = new ComboBox {
            Left = Scale(86), Top = y, Width = Scale(110), DropDownStyle = ComboBoxStyle.DropDownList,
            Font = new Font("Segoe UI", Scale(11), FontStyle.Regular, GraphicsUnit.Pixel) };
        theme.Items.AddRange(new object[] { "System", "Light", "Dark" });
        theme.SelectedIndex = _cfg.Theme switch { "light" => 1, "dark" => 2, _ => 0 };
        theme.SelectedIndexChanged += (_, __) =>
        {
            _cfg.Theme = theme.SelectedIndex switch { 1 => "light", 2 => "dark", _ => "system" };
            _cfg.Save(); ApplyTheme(); Build(); Refill(); RoundAndBlur();
        };
        _body.Controls.Add(theme);
        y += Scale(30);

        // updating Rafiq itself
        var upLabel = new Label {
            Text = $"Rafiq {Updater.Current}" + (_dev.Version.Length > 0 ? $"   ·   robot {_dev.Version}" : ""),
            Left = 0, Top = y + Scale(3), Width = _body.Width - Scale(70), Height = Scale(18),
            Font = new Font("Segoe UI", Scale(11), FontStyle.Regular, GraphicsUnit.Pixel),
            ForeColor = _skin.Text, BackColor = Color.Transparent };
        _body.Controls.Add(upLabel);
        var upBtn = Chip("Check", _body.Width - Scale(64), y, Scale(64), Scale(24), () => { });
        upBtn.Click += async (_, __) =>
        {
            upBtn.Enabled = false;
            upLabel.Text = "Looking...";
            await _up.Check(andInstall: _up.Phase == Updater.State.Found);
            upLabel.Text = _up.Phase switch
            {
                Updater.State.UpToDate => "You are up to date",
                Updater.State.Found    => $"Version {_up.Offered} is available",
                Updater.State.Failed   => _up.Error,
                _                      => "Working..."
            };
            upBtn.Text = _up.Phase == Updater.State.Found ? "Install" : "Check";
            upBtn.Enabled = true;
        };
    }
}
