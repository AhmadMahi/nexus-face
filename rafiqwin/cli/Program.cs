using Rafiq.Core;

// rafiq: a one line way for anything on this PC to reach the robot.
//
//   rafiq "build passed"
//   rafiq focus 25
//   rafiq toast "deploy done" copy
//   rafiq sleep
//
// It reads the address and token the tray app already has, so there is
// nothing separate to configure and no second copy of the token anywhere.

var cfg = Store.Load();
var dev = new Device(cfg);

if (args.Length == 0)
{
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
        case "sleep":
            await dev.DeepSleep();
            break;
        case "update":
            await dev.CheckUpdate();
            break;
        default:
            await dev.Say(string.Join(' ', args));
            break;
    }
}
catch (Exception e)
{
    Console.Error.WriteLine("rafiq: " + e.Message);
    return 1;
}

// Device swallows its own transport errors into a status line, so that is
// what decides the exit code rather than an exception.
if (dev.Status.Contains("Pair")) { Console.Error.WriteLine("rafiq: not paired with this robot."); return 3; }
if (dev.Status.Contains("reach")) { Console.Error.WriteLine("rafiq: could not reach it."); return 1; }
return 0;
