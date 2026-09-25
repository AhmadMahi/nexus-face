using System.Net;
using System.Net.Sockets;
using System.Text;

namespace Rafiq.Core;

/// <summary>
/// Everything that talks to the robot. It serves plain HTTP on the local
/// network, so nothing here should ever leave the house.
/// </summary>
public sealed class Device
{
    public Store Cfg { get; }
    readonly HttpClient _http;

    public Device(Store cfg)
    {
        Cfg = cfg;
        _http = new HttpClient { Timeout = TimeSpan.FromSeconds(6) };
    }

    public string Token { get => Store.GetToken(); set => Store.SetToken(value); }

    // what the robot last said about itself
    public bool? Reachable { get; private set; }
    public bool Paired, Linked, Following, Relaxing;
    public int FocusLeft, DndLeft;
    public string Version = "";
    public string Status = "";
    public event Action? Changed;

    DateTime _statusUntil = DateTime.MinValue;

    public void Flash(string s)
    {
        Status = s;
        _statusUntil = DateTime.UtcNow.AddSeconds(2.2);
        Changed?.Invoke();
    }
    public void TickStatus()
    {
        if (Status.Length > 0 && DateTime.UtcNow > _statusUntil) { Status = ""; Changed?.Invoke(); }
    }

    // ---- text ----

    public const int MaxLen = 84;

    /// <summary>
    /// The firmware keeps 84 bytes and cuts on a byte boundary, which would
    /// split a multi byte character in half. Trim by bytes here so it never
    /// has to. The panel draws ASCII, so anything fancier arrives intact but
    /// shows as blanks.
    /// </summary>
    public static string Clip(string s)
    {
        if (Encoding.UTF8.GetByteCount(s) <= MaxLen) return s;
        var sb = new StringBuilder();
        int n = 0;
        var e = System.Globalization.StringInfo.GetTextElementEnumerator(s);
        while (e.MoveNext())
        {
            var g = (string)e.Current;
            int c = Encoding.UTF8.GetByteCount(g);
            if (n + c > MaxLen - 3) break;      // three bytes for the ellipsis
            sb.Append(g); n += c;
        }
        return sb.ToString() + "…";
    }

    // ---- what the tiles do ----

    public Task Say(string raw)
    {
        var t = raw.Trim();
        if (t.Length == 0) return Task.CompletedTask;
        return Run("/api/msg", new() { ["m"] = Clip(t.Replace("\n", " ")) }, "Sent");
    }

    public Task Toast(string text, string kind, int seconds = 4) =>
        Run("/api/toast", new() { ["m"] = Clip(text), ["k"] = kind, ["s"] = seconds.ToString() }, null);

    public async Task StartFocus(int m)  { await Run("/api/focus", new() { ["m"] = m.ToString() }, $"Focus for {m} min"); FocusLeft = m * 60; }
    public async Task StopFocus()        { await Run("/api/focus", new() { ["m"] = "0" }, "Focus stopped"); FocusLeft = 0; }
    public async Task StartBreak(int m)  { await Run("/api/dnd", new() { ["m"] = m.ToString() }, $"On a break for {m} min"); DndLeft = m * 60; }
    public async Task EndBreak()         { await Run("/api/dnd", new() { ["m"] = "0" }, "Back"); DndLeft = 0; }
    public async Task SetRelax(bool on)  { await Run("/api/relax", new() { ["a"] = on ? "1" : "0" }, on ? "Resting" : "Back to normal"); Relaxing = on; }
    public async Task SetFollow(bool on) { await Run("/api/follow", new() { ["a"] = on ? "1" : "0" }, on ? "Watching the pointer" : "Eyes off"); Following = on; }
    public Task SetBusy(bool cam, bool mic) =>
        Run("/api/busy", new() { ["cam"] = cam ? "1" : "0", ["mic"] = mic ? "1" : "0" }, null);
    public Task Remind(string text) => Toast(text, "remind", 25);
    public Task CheckUpdate()       => Run("/api/update", new(), "Looking for an update");
    public async Task DeepSleep()   { await Run("/api/deepsleep", new(), "Going to sleep"); Linked = false; }

    public Task Canvas(byte[] px, int seconds = 8)
    {
        if (px.Length != 1024) { Flash("A screen is 1024 bytes"); return Task.CompletedTask; }
        return Run("/api/canvas",
                   new() { ["b"] = Convert.ToBase64String(px), ["s"] = seconds.ToString() }, "Drawn");
    }

    // ---- pairing ----

    public bool Pairing;
    public string PairError = "";

    /// <summary>
    /// Asking for a code needs no token, otherwise a robot whose token you
    /// had lost could never be paired again. Reading the code still means
    /// standing in front of it, which is the whole point.
    /// </summary>
    public async Task RequestCode()
    {
        if (Cfg.Ip.Length == 0) { Flash("Set the address first"); return; }
        PairError = "";
        try { await Post("/api/paircode", new()); Pairing = true; }
        catch { PairError = "Could not reach it"; }
        Changed?.Invoke();
    }

    public async Task Pair(string code)
    {
        var digits = new string(code.Where(char.IsDigit).ToArray());
        if (digits.Length != 6) { PairError = "Six digits"; Changed?.Invoke(); return; }
        try
        {
            var body = await Post("/api/pair", new() { ["c"] = digits });
            var t = Json.Str(body, "token");
            if (string.IsNullOrEmpty(t)) { PairError = "Wrong code"; Changed?.Invoke(); return; }
            Token = t; Paired = true; Pairing = false; PairError = "";
            Flash("Paired");
            await Refresh();
        }
        catch { PairError = "Wrong code, or it expired"; Changed?.Invoke(); }
    }

    public async Task Unpair()
    {
        await Run("/api/unpair", new(), "Unpaired");
        Token = ""; Paired = false;
    }

    // ---- state ----

    public async Task Refresh()
    {
        if (Cfg.Ip.Length == 0) { Reachable = null; Changed?.Invoke(); return; }
        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Get, Url("/api/state"));
            req.Headers.TryAddWithoutValidation("X-Rafiq-App", "1");
            var tok = Token;
            if (tok.Length > 0) req.Headers.TryAddWithoutValidation("X-Rafiq-Token", tok);
            using var resp = await _http.SendAsync(req);
            int code = (int)resp.StatusCode;
            // 401 still means it answered, so the tray stays green and the
            // panel shows the pairing prompt rather than a dead robot.
            Reachable = code == 200 || code == 401;
            if (code == 401) { Paired = true; Linked = false; Changed?.Invoke(); return; }
            if (code != 200) { Changed?.Invoke(); return; }
            var s = await resp.Content.ReadAsStringAsync();
            Paired    = Json.Bool(s, "paired");
            Linked    = Json.Bool(s, "linked");
            Following = Json.Bool(s, "follow");
            Relaxing  = Json.Bool(s, "relax");
            FocusLeft = Json.Int(s, "focusLeft");
            DndLeft   = Json.Int(s, "dndLeft");
            var v = Json.Str(s, "fw");
            if (!string.IsNullOrEmpty(v)) Version = v;
        }
        catch { Reachable = false; Linked = false; }
        Changed?.Invoke();
    }

    // ---- transport ----

    async Task Run(string path, Dictionary<string, string> fields, string? say)
    {
        if (Cfg.Ip.Length == 0) { Flash("Set the address first"); return; }
        try
        {
            await Post(path, fields);
            Reachable = true;
            if (say != null) Flash(say);
        }
        catch (UnauthorizedAccessException) { Reachable = true; Flash("Pair with the robot first"); }
        catch { Reachable = false; Flash("Could not reach it"); }
        Changed?.Invoke();
    }

    Uri Url(string path) => new($"http://{Cfg.Ip.Trim()}{path}");

    async Task<string> Post(string path, Dictionary<string, string> fields)
    {
        using var req = new HttpRequestMessage(HttpMethod.Post, Url(path));
        req.Headers.TryAddWithoutValidation("X-Rafiq-App", "1");
        var tok = Token;
        if (tok.Length > 0) req.Headers.TryAddWithoutValidation("X-Rafiq-Token", tok);
        req.Content = new StringContent(Form(fields), Encoding.UTF8,
                                        "application/x-www-form-urlencoded");
        using var resp = await _http.SendAsync(req);
        int code = (int)resp.StatusCode;
        if (code is 401 or 403) throw new UnauthorizedAccessException();
        if (code != 200) throw new HttpRequestException($"status {code}");
        return await resp.Content.ReadAsStringAsync();
    }

    /// <summary>
    /// The device parses a plain form body, so everything outside the
    /// unreserved set has to be escaped, including the plus sign that a
    /// naive encoder would leave to be read back as a space.
    /// </summary>
    public static string Form(Dictionary<string, string> fields)
    {
        const string safe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
        var sb = new StringBuilder();
        foreach (var (k, v) in fields)
        {
            if (sb.Length > 0) sb.Append('&');
            sb.Append(k).Append('=');
            foreach (var b in Encoding.UTF8.GetBytes(v))
            {
                char c = (char)b;
                if (safe.IndexOf(c) >= 0) sb.Append(c);
                else sb.Append('%').Append(b.ToString("X2"));
            }
        }
        return sb.ToString();
    }
}

/// <summary>
/// Small readers rather than a full parser: the device hand rolls its JSON
/// and one unexpected field should never cost us the rest of it.
/// </summary>
public static class Json
{
    public static string Str(string b, string key)
    {
        int k = b.IndexOf("\"" + key + "\"", StringComparison.Ordinal);
        if (k < 0) return "";
        int i = k + key.Length + 2;
        while (i < b.Length && (b[i] == ':' || b[i] == ' ')) i++;
        if (i >= b.Length || b[i] != '"') return "";
        i++;
        int e = b.IndexOf('"', i);
        return e < 0 ? "" : b.Substring(i, e - i);
    }
    public static string Raw(string b, string key)
    {
        int k = b.IndexOf("\"" + key + "\"", StringComparison.Ordinal);
        if (k < 0) return "";
        int i = k + key.Length + 2;
        while (i < b.Length && (b[i] == ':' || b[i] == ' ')) i++;
        int s = i;
        while (i < b.Length && b[i] != ',' && b[i] != '}') i++;
        return b.Substring(s, i - s).Trim();
    }
    public static bool Bool(string b, string key) => Raw(b, key) == "true";
    public static int  Int(string b, string key)  => int.TryParse(Raw(b, key), out var v) ? v : 0;
}

/// <summary>
/// Where the pointer is, sent as bare datagrams. Ten a second through a
/// fresh handshake each time would drown the robot, and a lost reading
/// costs nothing because another is a tenth of a second behind it.
/// </summary>
public sealed class PointerStream : IDisposable
{
    UdpClient? _udp;
    IPEndPoint? _to;

    public void Start(string host)
    {
        Stop();
        var bare = host.Split(':')[0].Trim();
        if (!IPAddress.TryParse(bare, out var ip))
        {
            try { ip = Dns.GetHostAddresses(bare).First(a => a.AddressFamily == AddressFamily.InterNetwork); }
            catch { return; }
        }
        _udp = new UdpClient();
        _to = new IPEndPoint(ip, 4210);
    }

    public void Send(int x, int y)
    {
        if (_udp == null || _to == null) return;
        var b = Encoding.ASCII.GetBytes($"{x} {y}");
        try { _udp.Send(b, b.Length, _to); } catch { }
    }

    public void Stop() { _udp?.Dispose(); _udp = null; _to = null; }
    public void Dispose() => Stop();
}
