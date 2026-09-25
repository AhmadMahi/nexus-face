using System.Net;
using System.Text;
using Rafiq.Core;

// Runs on a real Windows machine in CI. Everything here is either pure
// logic or a stand-in robot on localhost, so it needs no hardware and no
// screen, but it does exercise the shipping code rather than a copy.

int fails = 0;
void Check(bool ok, string what)
{
    Console.WriteLine((ok ? "  ok    " : "  FAIL  ") + what);
    if (!ok) fails++;
}

Console.WriteLine("text clipping");
Check(Device.Clip("hello") == "hello", "short text is untouched");
{
    var long84 = new string('x', 200);
    var c = Device.Clip(long84);
    Check(Encoding.UTF8.GetByteCount(c) <= 84, $"200 chars trimmed to {Encoding.UTF8.GetByteCount(c)} bytes");
    Check(c.EndsWith("…"), "and marked as trimmed");
}
{
    // every one of these is three bytes, so a naive cut at 84 would slice
    // one in half and make the whole thing invalid
    var cjk = string.Concat(Enumerable.Repeat("日", 60));
    var c = Device.Clip(cjk);
    var bytes = Encoding.UTF8.GetBytes(c);
    Check(bytes.Length <= 84, $"wide text fits in {bytes.Length} bytes");
    Check(Encoding.UTF8.GetString(bytes) == c, "and is still valid text");
}

Console.WriteLine("\nform encoding, which the firmware parses by hand");
{
    var f = Device.Form(new() { ["m"] = "a & b = c ? d # e + f %" });
    Check(!f.Contains('&') || f.IndexOf('&') > 2, "ampersand is escaped, not a separator");
    Check(f.Contains("%2B"), "plus is escaped, or it reads back as a space");
    Check(f.Contains("%26") && f.Contains("%3D") && f.Contains("%23"), "and so are & = #");
    var round = WebUtility.UrlDecode(f.Substring(2));
    Check(round == "a & b = c ? d # e + f %", "it decodes back to exactly what went in");
}
{
    var f = Device.Form(new() { ["m"] = "café 日本 ✓" });
    Check(WebUtility.UrlDecode(f.Substring(2)) == "café 日本 ✓", "unicode round trips");
}

Console.WriteLine("\nreading what the robot says");
{
    var s = "{\"paired\":true,\"linked\":false,\"focusLeft\":1500,\"fw\":\"2.3.0\",\"dndLeft\":0}";
    Check(Json.Bool(s, "paired"), "a true is read");
    Check(!Json.Bool(s, "linked"), "a false is read");
    Check(Json.Int(s, "focusLeft") == 1500, "a number is read");
    Check(Json.Str(s, "fw") == "2.3.0", "a string is read");
    Check(Json.Int(s, "nothingHere") == 0, "a missing key is zero, not a crash");
}

Console.WriteLine("\nversion comparison for self update");
Check(Updater.Newer("1.2.0", "1.1.0"), "1.2.0 beats 1.1.0");
Check(Updater.Newer("1.10.0", "1.9.0"), "1.10.0 beats 1.9.0, not the other way");
Check(!Updater.Newer("1.1.0", "1.1.0"), "the same version is not newer");
Check(!Updater.Newer("1.0.9", "1.1.0"), "1.0.9 does not beat 1.1.0");
Check(Updater.Newer("2.0.0", "1.99.99"), "2.0.0 beats 1.99.99");

Console.WriteLine("\nonly GitHub is trusted as a download");
foreach (var (host, want) in new[] {
    ("github.com", true), ("objects.githubusercontent.com", true),
    ("evil.example.com", false), ("github.com.evil.net", false) })
{
    var ok = Updater.IsGitHub(new Uri($"https://{host}/x/Rafiq.zip"));
    Check(ok == want, $"{host} is {(ok ? "allowed" : "refused")}");
}
Check(!Updater.IsGitHub(new Uri("http://github.com/x.zip")), "plain http is refused");

Console.WriteLine("\nidle time and the camera light");
{
    var idle = Session.IdleSeconds();
    Check(idle >= 0 && idle < 86400, $"idle reads sanely ({idle:F1}s)");
    // These must answer without throwing and without asking permission,
    // whatever the machine happens to be doing.
    var cam = AvWatch.CameraLive();
    var mic = AvWatch.MicLive();
    Console.WriteLine($"         camera live: {cam}, mic live: {mic}");
    Check(true, "the camera and mic flags read without error");
    Check(Theme.SystemIsLight() || true, "the system theme reads without error");
}

Console.WriteLine("\nreminders keep themselves in order");
{
    var dir = Store.Dir;
    var r = new Reminders();
    foreach (var x in r.Pending.ToList()) r.Remove(x);
    r.AddInMinutes("first", 30);
    r.AddInMinutes("second", 5);
    r.AddInMinutes("third", 90);
    Check(r.Pending.Select(x => x.Text).SequenceEqual(new[] { "second", "first", "third" }),
          "soonest first");
    r.Remove(r.Pending[0]);
    Check(r.Pending.Select(x => x.Text).SequenceEqual(new[] { "first", "third" }), "one can be dropped");
    var fired = new List<string>();
    r.OnDue = x => fired.Add(x.Text);
    r.Add("now", DateTime.Now.AddSeconds(-1));
    r.Tick();
    Check(fired.Contains("now"), "one that is due fires");
    r.Tick();
    Check(fired.Count(x => x == "now") == 1, "and only fires once");
    foreach (var x in r.Pending.ToList()) r.Remove(x);
}

Console.WriteLine("\nthe token is kept encrypted, not in the settings file");
{
    Store.SetToken("a-secret-token-value");
    Check(Store.GetToken() == "a-secret-token-value", "it comes back out");
    var blob = File.ReadAllBytes(Path.Combine(Store.Dir, "token.bin"));
    Check(!Encoding.UTF8.GetString(blob).Contains("a-secret-token-value"),
          "and is not sitting there in plain text");
    Store.SetToken("");
    Check(Store.GetToken() == "", "clearing it works");
}

Console.WriteLine("\nwhat can run beside what");
{
    var c = new Store();
    var dv = new Device(c);
    Check(dv.Blocked(Device.Tool.BreakNow) == null, "nothing running: on a break is free");
    Check(dv.Blocked(Device.Tool.DeepSleep) == null, "nothing running: deep sleep is free");

    dv.FocusLeft = 900;
    Check(dv.FocusRunning, "focus reads as running");
    Check(dv.Blocked(Device.Tool.BreakNow) == "during focus", "on a break is blocked, and says why");
    Check(dv.Blocked(Device.Tool.DeepSleep) == "after focus", "deep sleep is blocked, and says why");
    Check(dv.Blocked(Device.Tool.Relax) == null, "relax still allowed beside focus");
    Check(dv.Blocked(Device.Tool.Follow) == null, "follow still allowed beside focus");
    dv.FocusLeft = 0;

    dv.Following = true;
    Check(dv.Blocked(Device.Tool.Relax) == "following", "relax is blocked while following");
    Check(dv.Blocked(Device.Tool.Follow) == null, "follow stays clickable, so it can be turned off");
    dv.Following = false; dv.Relaxing = true;
    Check(dv.Blocked(Device.Tool.Follow) == "relaxing", "follow is blocked while relaxing");
    dv.Relaxing = false;

    dv.DndLeft = 600;
    Check(dv.Blocked(Device.Tool.DeepSleep) == "on a break", "deep sleep waits for a break");
    dv.DndLeft = 0;
}

Console.WriteLine("\nend to end against a stand-in robot");
{
    var listener = new HttpListener();
    listener.Prefixes.Add("http://127.0.0.1:8099/");
    listener.Start();
    var seen = new List<(string path, string body)>();
    var token = "tok-" + Guid.NewGuid().ToString("N");
    int code = 123456;

    var pump = Task.Run(async () =>
    {
        while (listener.IsListening)
        {
            HttpListenerContext ctx;
            try { ctx = await listener.GetContextAsync(); } catch { break; }
            var path = ctx.Request.Url!.AbsolutePath;
            var body = new StreamReader(ctx.Request.InputStream).ReadToEnd();
            seen.Add((path, body));

            string reply = "{\"ok\":true}";
            int status = 200;
            if (path == "/api/pair")
                reply = body.Contains(code.ToString()) ? $"{{\"ok\":true,\"token\":\"{token}\"}}"
                                                       : "{\"ok\":false}";
            else if (path == "/api/state")
                reply = "{\"paired\":true,\"linked\":true,\"follow\":false,\"relax\":false," +
                        "\"focusLeft\":1500,\"dndLeft\":1200,\"fw\":\"2.3.0\"}";
            else if (path != "/api/paircode" && ctx.Request.Headers["X-Rafiq-Token"] != token)
            { status = 401; reply = "{\"ok\":false}"; }

            var b = Encoding.UTF8.GetBytes(reply);
            ctx.Response.StatusCode = status;
            ctx.Response.ContentLength64 = b.Length;
            ctx.Response.OutputStream.Write(b);
            ctx.Response.Close();
        }
    });

    var cfg = new Store { Ip = "127.0.0.1:8099" };
    Store.SetToken("");
    var dev = new Device(cfg);

    await dev.Pair("000000");
    Check(!dev.Paired, "a wrong code is refused");
    await dev.Pair(code.ToString());
    Check(dev.Paired && dev.Token == token, "the right code pairs and stores the token");

    await dev.Say("hello there");
    await dev.StartFocus(25);
    await dev.StartBreak(20);
    await dev.SetBusy(true, false);
    await dev.Remind("stand up");
    await dev.Canvas(new byte[1024], 10);

    string? BodyOf(string p) => seen.LastOrDefault(x => x.path == p).body;
    Check(BodyOf("/api/msg")!.Contains("m=hello%20there"), "the message text arrives whole");
    Check(BodyOf("/api/focus") == "m=25", "focus carries the minutes");
    Check(BodyOf("/api/dnd") == "m=20", "a break carries the minutes");
    Check(BodyOf("/api/busy")!.Contains("cam=1") && BodyOf("/api/busy")!.Contains("mic=0"),
          "camera on, mic off");
    Check(BodyOf("/api/toast")!.Contains("k=remind"), "a reminder is marked as one");
    Check(BodyOf("/api/canvas")!.Length > 1300, "a full screen of pixels goes out");

    await dev.Refresh();
    Check(dev.FocusLeft == 1500 && dev.DndLeft == 1200, "both countdowns are read back");
    Check(dev.Version == "2.3.0", "the firmware version is read back");

    // and the token has to actually matter
    var good = dev.Token;
    dev.Token = "wrong";
    await dev.Say("should be refused");
    Check(dev.Status.Contains("Pair"), "a wrong token is turned away");
    dev.Token = good;
    await dev.Say("allowed again");
    Check(dev.Status == "Sent", "the right token is let through");

    listener.Stop();
    Store.SetToken("");
}

Console.WriteLine();
Console.WriteLine(fails == 0 ? "PASS: 0 failures" : $"{fails} FAILURE(S)");
return fails == 0 ? 0 : 1;
