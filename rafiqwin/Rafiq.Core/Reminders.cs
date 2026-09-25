using System.Text.Json;

namespace Rafiq.Core;

public sealed class Reminder
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public string Text { get; set; } = "";
    public DateTime FireAt { get; set; }
    public bool Done { get; set; }
}

/// <summary>
/// A short list of things to be told about, held here rather than on the
/// robot. The robot sleeps and loses track of time; this machine does not,
/// so it keeps the list and simply says the word when one comes due.
/// </summary>
public sealed class Reminders
{
    public const int MaxKept = 20;
    static string Path => System.IO.Path.Combine(Store.Dir, "reminders.json");

    readonly List<Reminder> _items = new();
    public Action<Reminder>? OnDue;

    public Reminders() => Load();

    public IReadOnlyList<Reminder> Pending =>
        _items.Where(r => !r.Done).OrderBy(r => r.FireAt).ToList();

    public void Add(string text, DateTime when)
    {
        var t = text.Trim();
        if (t.Length == 0) return;
        _items.Add(new Reminder { Text = t, FireAt = when });
        Trim();
        Save();
    }

    public void AddInMinutes(string text, int m) =>
        Add(text, DateTime.Now.AddMinutes(Math.Max(1, m)));

    public void Remove(Reminder r) { _items.RemoveAll(x => x.Id == r.Id); Save(); }

    public void Tick()
    {
        var now = DateTime.Now;
        foreach (var r in _items.Where(r => !r.Done && r.FireAt <= now).ToList())
        {
            r.Done = true;
            OnDue?.Invoke(r);
        }
        // Anything long since said is cleared out, so the list stays the
        // things still ahead of you rather than a history.
        _items.RemoveAll(r => r.Done && r.FireAt < now.AddHours(-1));
        Save();
    }

    void Trim()
    {
        var extra = Pending.Count - MaxKept;
        if (extra <= 0) return;
        foreach (var r in Pending.TakeLast(extra).ToList()) _items.RemoveAll(x => x.Id == r.Id);
    }

    void Save()
    {
        try
        {
            Directory.CreateDirectory(Store.Dir);
            File.WriteAllText(Path, JsonSerializer.Serialize(_items));
        }
        catch { }
    }
    void Load()
    {
        try
        {
            if (!File.Exists(Path)) return;
            var v = JsonSerializer.Deserialize<List<Reminder>>(File.ReadAllText(Path));
            if (v != null) _items.AddRange(v);
        }
        catch { }
    }
}
