using System.Diagnostics;
using System.Runtime.InteropServices;
using System.IO.Compression;
using System.Text.Json;

namespace Rafiq.Core;

/// <summary>
/// Updating Rafiq itself from the same GitHub repo the firmware comes from.
/// Windows builds are tagged win-vX.Y.Z so they sit beside the firmware's
/// vX.Y.Z tags and the Mac's app-vX.Y.Z without any of them colliding.
///
/// Worth being plain about what is and is not checked. The download comes
/// over HTTPS from a host that has to belong to GitHub, and the repository
/// is fixed in this file rather than read from anywhere. But this build is
/// not signed with a paid code signing certificate, so there is no
/// signature to verify beyond that. The trust is the transport and the
/// pinned repository.
/// </summary>
public sealed class Updater
{
    public const string Repo = "AhmadMahi/nexus-face";
    const string TagPrefix = "win-v";

    public enum State { Idle, Checking, UpToDate, Found, Downloading, Installing, Failed }
    public State Phase { get; private set; } = State.Idle;
    public string Offered { get; private set; } = "";
    public string Error { get; private set; } = "";

    public static string Current =>
        (System.Reflection.Assembly.GetEntryAssembly()?.GetName().Version is { } v)
            ? $"{v.Major}.{v.Minor}.{v.Build}" : "0";

    public async Task Check(bool andInstall)
    {
        Phase = State.Checking; Error = "";
        try
        {
            var rel = await Latest();
            if (rel == null || !Newer(rel.Value.version, Current)) { Phase = State.UpToDate; return; }
            Offered = rel.Value.version;
            if (!andInstall) { Phase = State.Found; return; }
            await Install(rel.Value);
        }
        catch { Phase = State.Failed; Error = "Could not check"; }
    }

    // ---------------------------------------------------------------

    public static bool IsGitHub(Uri u)
    {
        if (u.Scheme != "https") return false;
        var h = u.Host.ToLowerInvariant();
        return h == "github.com" || h.EndsWith(".github.com")
            || h == "objects.githubusercontent.com" || h.EndsWith(".githubusercontent.com");
    }

    public static bool Newer(string a, string b)
    {
        var x = a.Split('.').Select(s => int.TryParse(s, out var n) ? n : 0).ToArray();
        var y = b.Split('.').Select(s => int.TryParse(s, out var n) ? n : 0).ToArray();
        for (int i = 0; i < Math.Max(x.Length, y.Length); i++)
        {
            int l = i < x.Length ? x[i] : 0, r = i < y.Length ? y[i] : 0;
            if (l != r) return l > r;
        }
        return false;
    }

    async Task<(string version, Uri zip)?> Latest()
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(15) };
        http.DefaultRequestHeaders.TryAddWithoutValidation("User-Agent", "Rafiq");
        http.DefaultRequestHeaders.TryAddWithoutValidation("Accept", "application/vnd.github+json");
        var body = await http.GetStringAsync($"https://api.github.com/repos/{Repo}/releases?per_page=30");
        using var doc = JsonDocument.Parse(body);

        (string version, Uri zip)? best = null;
        foreach (var r in doc.RootElement.EnumerateArray())
        {
            if (!r.TryGetProperty("tag_name", out var tagEl)) continue;
            var tag = tagEl.GetString() ?? "";
            if (!tag.StartsWith(TagPrefix, StringComparison.Ordinal)) continue;
            if (r.TryGetProperty("draft", out var d) && d.GetBoolean()) continue;
            var v = tag[TagPrefix.Length..];
            if (!r.TryGetProperty("assets", out var assets)) continue;
            foreach (var a in assets.EnumerateArray())
            {
                var name = a.GetProperty("name").GetString() ?? "";
                if (!name.EndsWith(Arch(), StringComparison.OrdinalIgnoreCase)) continue;
                var url = a.GetProperty("browser_download_url").GetString() ?? "";
                if (!Uri.TryCreate(url, UriKind.Absolute, out var u) || !IsGitHub(u)) continue;
                if (best == null || Newer(v, best.Value.version)) best = (v, u);
                break;
            }
        }
        return best;
    }

    /// <summary>Only the build for this machine is ever offered.</summary>
    public static string Arch() =>
        RuntimeInformation.ProcessArchitecture == Architecture.Arm64 ? "win-arm64.zip" : "win-x64.zip";

    // ---------------------------------------------------------------

    async Task Install((string version, Uri zip) rel)
    {
        Phase = State.Downloading;
        var work = Path.Combine(Path.GetTempPath(), "rafiq-update-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(work);
        var zip = Path.Combine(work, "rafiq.zip");

        using (var http = new HttpClient { Timeout = TimeSpan.FromMinutes(5) })
        {
            http.DefaultRequestHeaders.TryAddWithoutValidation("User-Agent", "Rafiq");
            var bytes = await http.GetByteArrayAsync(rel.zip);
            await File.WriteAllBytesAsync(zip, bytes);
        }

        Phase = State.Installing;
        var staged = Path.Combine(work, "new");
        ZipFile.ExtractToDirectory(zip, staged);
        if (!File.Exists(Path.Combine(staged, "Rafiq.exe")))
        {
            Phase = State.Failed; Error = "The download had no Rafiq in it"; return;
        }

        var here = AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
        var pid = Environment.ProcessId;

        // A running program cannot replace its own folder, so a small script
        // waits for this process to go and then does the swap. The old copy
        // is moved aside rather than deleted, and put back if anything
        // fails: deleting first would mean one bad copy left no Rafiq.
        var cmd = Path.Combine(work, "swap.cmd");
        await File.WriteAllTextAsync(cmd, $"""
        @echo off
        setlocal
        set "DEST={here}"
        set "NEW={staged}"
        set "BAK={here}.replacing"
        for /L %%i in (1,1,100) do (
          tasklist /FI "PID eq {pid}" 2>nul | find "{pid}" >nul || goto gone
          ping -n 1 -w 200 127.0.0.1 >nul
        )
        :gone
        if exist "%BAK%" rmdir /S /Q "%BAK%"
        if exist "%DEST%" move "%DEST%" "%BAK%" || exit /b 1
        xcopy /E /I /Y /Q "%NEW%" "%DEST%" >nul
        if errorlevel 1 (
          if exist "%DEST%" rmdir /S /Q "%DEST%"
          if exist "%BAK%" move "%BAK%" "%DEST%"
          start "" "%DEST%\Rafiq.exe"
          exit /b 1
        )
        if exist "%BAK%" rmdir /S /Q "%BAK%"
        start "" "%DEST%\Rafiq.exe"
        rmdir /S /Q "{work}"
        """);

        Process.Start(new ProcessStartInfo("cmd.exe", $"/c \"{cmd}\"")
        {
            CreateNoWindow = true,
            UseShellExecute = false
        });
        Environment.Exit(0);
    }
}
