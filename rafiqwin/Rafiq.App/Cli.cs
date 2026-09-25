using System.Runtime.InteropServices;
using Rafiq.Core;

namespace Rafiq.App;

/// <summary>
/// The command line side of the same binary.
///
/// It lives here rather than in an exe of its own because Windows file
/// names ignore case: a rafiq.exe next to Rafiq.exe is the same file, and
/// shipping both means one quietly replacing the other. One binary, two
/// ways in, and a rafiq.cmd beside it so the command still reads as rafiq.
/// </summary>
static class Cli
{
    [DllImport("kernel32.dll")] static extern bool AttachConsole(int pid);
    const int ParentProcess = -1;

    public static async Task<int> Run(string[] args)
    {
        // A windowed program has no console of its own, so it borrows the
        // one it was typed into. Without this, nothing it prints is seen.
        AttachConsole(ParentProcess);

        var cfg = Store.Load();
        var dev = new Device(cfg);

        if (args.Length == 0 || args[0] is "-h" or "--help" or "/?")
        {
            Console.WriteLine();
            Console.WriteLine("""
            rafiq  -  send something to the robot

              rafiq "build passed"         put it on the screen
              rafiq toast "saved" [kind]   show it briefly (copy, paste, break, remind)
              rafiq focus <minutes>        start a focus run, 0 to stop
              rafiq break <minutes>        on a break, and lock this PC
              rafiq relax on|off           the screensaver
              rafiq sleep                  deep sleep, power to wake it
              rafiq update                 look for new firmware
            """);
            return 0;
        }

        if (cfg.Ip.Length == 0)
        {
            Console.Error.WriteLine("rafiq: no address set. Open Rafiq and set one.");
            return 2;
        }

        try
        {
            switch (args[0])
            {
                case "toast":
                    await dev.Toast(args.Length > 1 ? args[1] : "",
                                    args.Length > 2 ? args[2] : "note", 5);
                    break;
                case "focus":
                    await dev.StartFocus(args.Length > 1 && int.TryParse(args[1], out var f) ? f : 25);
                    break;
                case "break":
                    await dev.StartBreak(args.Length > 1 && int.TryParse(args[1], out var b) ? b : 15);
                    Session.Lock();
                    break;
                case "relax":
                    await dev.SetRelax(!(args.Length > 1 && args[1] == "off"));
                    break;
                case "sleep":  await dev.DeepSleep();   break;
                case "update": await dev.CheckUpdate(); break;
                default:       await dev.Say(string.Join(' ', args)); break;
            }
        }
        catch (Exception e)
        {
            Console.Error.WriteLine("rafiq: " + e.Message);
            return 1;
        }

        // Device folds its transport errors into a status line rather than
        // throwing, so that is what decides the exit code.
        if (dev.Status.Contains("Pair")) { Console.Error.WriteLine("rafiq: not paired with this robot."); return 3; }
        if (dev.Status.Contains("reach")) { Console.Error.WriteLine("rafiq: could not reach it."); return 1; }
        Console.WriteLine("sent");
        return 0;
    }
}
