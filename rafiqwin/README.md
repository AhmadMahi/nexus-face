# Rafiq for Windows

The tray companion for the robot, matching the macOS app.

## Running it

Download the zip for your machine from
[Releases](https://github.com/AhmadMahi/nexus-face/releases), right-click it,
Properties, tick **Unblock**, then extract it anywhere and run `Rafiq.exe`.
It sits in the tray. Nothing needs installing first.

Windows 10 or 11. Both Intel and ARM builds are published.

## Layout

    Rafiq.Core    everything testable: the API, reminders, settings,
                  the updater, and the Windows watchers
    Rafiq.App     the tray icon and the panel
    Rafiq.Tests   runs in CI on a real Windows machine
    cli           the `rafiq` command

## Building it yourself

    dotnet build rafiqwin/Rafiq.App/Rafiq.App.csproj -c Release
    dotnet run   --project rafiqwin/Rafiq.Tests/Rafiq.Tests.csproj

## How it differs from the Mac version

Locking the screen is a plain documented Windows call, `LockWorkStation`,
rather than the private one macOS now needs.

The camera and microphone light reads the same privacy record Windows shows
you under Settings, so it reports whether **any** app is using a device. The
Mac version can narrow that to the built-in camera and microphone, because
macOS exposes it per device; Windows records it per application instead.
Anything reaching the hardware around the Windows privacy framework would
not show up.

Clipboard mirroring honours `ExcludeClipboardContentFromMonitorProcessing`,
`CanIncludeInClipboardHistory` and `CanUploadToCloudClipboard`, which are the
formats password managers set so tools like this leave credentials alone.

The pairing token is encrypted with DPAPI under your account rather than
kept in the settings file.
