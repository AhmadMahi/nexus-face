using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Rafiq.Core;

/// <summary>
/// Settings on disk, and the pairing token kept apart from them.
///
/// The token is what stops anyone else on the network driving the robot, so
/// it is encrypted with DPAPI under the current user rather than sitting in
/// the same readable JSON as everything else.
/// </summary>
public sealed class Store
{
    public static readonly string Dir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Rafiq");

    static string SettingsPath => Path.Combine(Dir, "settings.json");
    static string TokenPath    => Path.Combine(Dir, "token.bin");

    public string Ip { get; set; } = "";
    public bool WatchClipboard { get; set; }
    public bool BreakOn { get; set; }
    public int  BreakMins { get; set; } = 45;
    public int  BreakCustom { get; set; } = 25;
    public bool LockWhenIdle { get; set; }
    public int  LockIdleMins { get; set; } = 5;
    public bool WatchAv { get; set; }
    public bool Follow { get; set; }
    public string Theme { get; set; } = "system";
    public string PhrasesRaw { get; set; } = "On my way\nBack in 5\nIn a meeting\nCall me\nDone";

    public IEnumerable<string> Phrases =>
        PhrasesRaw.Split('\n').Select(s => s.Trim()).Where(s => s.Length > 0);

    // ---- persistence ----

    public static Store Load()
    {
        try
        {
            if (File.Exists(SettingsPath))
                return JsonSerializer.Deserialize<Store>(File.ReadAllText(SettingsPath)) ?? new Store();
        }
        catch { /* a corrupt file should cost settings, never a launch */ }
        return new Store();
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Dir);
            File.WriteAllText(SettingsPath,
                JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch { }
    }

    // ---- the token ----

    public static string GetToken()
    {
        try
        {
            if (!File.Exists(TokenPath)) return "";
            var clear = ProtectedData.Unprotect(File.ReadAllBytes(TokenPath), null,
                                                DataProtectionScope.CurrentUser);
            return Encoding.UTF8.GetString(clear);
        }
        catch { return ""; }
    }

    public static void SetToken(string t)
    {
        try
        {
            Directory.CreateDirectory(Dir);
            if (string.IsNullOrEmpty(t)) { if (File.Exists(TokenPath)) File.Delete(TokenPath); return; }
            var blob = ProtectedData.Protect(Encoding.UTF8.GetBytes(t), null,
                                             DataProtectionScope.CurrentUser);
            File.WriteAllBytes(TokenPath, blob);
        }
        catch { }
    }
}
