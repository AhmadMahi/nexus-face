import SwiftUI
import AppKit

/// Starts the background work the moment the app launches rather than the
/// moment the panel is first opened. Without this the menu bar icon has
/// nothing to report until you click it, which is backwards for an icon
/// whose whole job is to be glanced at.
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ n: Notification) {
        MainActor.assumeIsolated { Services.shared.start() }
    }
}

@main
struct RafiqBarApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @ObservedObject private var dev = Device.shared

    var body: some Scene {
        MenuBarExtra {
            Panel()
                .environmentObject(Device.shared)
                .environmentObject(Services.shared)
        } label: {
            Image(nsImage: RobotIcon.image(face))
        }
        .menuBarExtraStyle(.window)
    }

    /// Red means it answered before and has stopped. Until the first reply
    /// there is nothing to report, so it stays grey rather than claiming a
    /// fault that has not happened.
    private var face: RobotIcon.State {
        if dev.ip.isEmpty { return .unset }
        switch dev.reachable {
        case .some(true):  return .linked
        case .some(false): return .adrift
        case .none:        return .unset
        }
    }
}

struct Panel: View {
    @EnvironmentObject var dev: Device
    @EnvironmentObject var svc: Services

    @State private var draft = ""
    @State private var showSettings = false
    @State private var showFocus = false
    @State private var showBreak = false
    @State private var showRemind = false
    @State private var showPhrases = false
    @FocusState private var typing: Bool
    @Environment(\.colorScheme) private var systemScheme

    var body: some View {
        VStack(alignment: .leading, spacing: 11) {
            header

            if dev.ip.isEmpty || showSettings {
                Settings(showing: $showSettings).environmentObject(dev)
            } else if dev.pairing {
                PairView().environmentObject(dev)
            } else if showFocus {
                Minutes(title: "Focus for", choices: [5, 10, 15, 25, 30, 45, 60, 90],
                        note: "The panel shows the countdown, then rests, then shows it "
                            + "again. It will not drop off until the time is up.",
                        showing: $showFocus) { m in
                    Task { await dev.startFocus(m) }
                }
            } else if showBreak {
                Minutes(title: "On a break for", choices: [5, 10, 15, 20, 30, 45, 60, 90],
                        note: "The robot holds the sign and your Mac locks straight away. "
                            + "The display comes back when the time is up.",
                        showing: $showBreak) { m in
                    Task {
                        await dev.startBreak(m)
                        try? await Task.sleep(nanoseconds: 400_000_000)
                        Screen.lock()
                    }
                }
            } else if showRemind {
                RemindSheet(showing: $showRemind).environmentObject(dev)
            } else if showPhrases {
                Phrases(showing: $showPhrases).environmentObject(dev)
            } else {
                grid
                compose
            }

            if !dev.status.isEmpty {
                Text(dev.status)
                    .font(.system(size: 10))
                    .foregroundStyle(.secondary)
                    .transition(.opacity)
            }
        }
        .padding(13)
        .frame(width: 320)
        .background(.ultraThinMaterial)
        .clipShape(RoundedRectangle(cornerRadius: 15, style: .continuous))
        .animation(.easeOut(duration: 0.18), value: showSettings)
        .animation(.easeOut(duration: 0.18), value: showFocus)
        .animation(.easeOut(duration: 0.18), value: showBreak)
        .animation(.easeOut(duration: 0.18), value: showRemind)
        .animation(.easeOut(duration: 0.18), value: showPhrases)
        // Both, deliberately. preferredColorScheme is a window level hint
        // and does not reliably reach a menu bar window; the environment
        // override is what actually decides how the colours resolve.
        .preferredColorScheme(dev.colorScheme)
        .environment(\.colorScheme, dev.colorScheme ?? systemScheme)
        .animation(.easeOut(duration: 0.18), value: dev.pairing)
        .onAppear { Task { await dev.refresh() } }
    }

    // ---------------------------------------------------------------

    private var header: some View {
        HStack(spacing: 7) {
            Circle()
                .fill(dot)
                .frame(width: 7, height: 7)
            Text("RAFIQ")
                .font(.system(size: 11, weight: .semibold))
                .tracking(1.4)
            if dev.focusLeft > 0 {
                Text("\(max(0, dev.focusLeft) / 60 + 1)m")
                    .font(.system(size: 10, weight: .medium))
                    .padding(.horizontal, 5).padding(.vertical, 1)
                    .background(Capsule().fill(Color.accentColor.opacity(0.25)))
                    .foregroundStyle(Color.accentColor)
            }
            Spacer()
            Button { showSettings.toggle(); showFocus = false } label: {
                Image(systemName: "gearshape").font(.system(size: 11))
            }
            .buttonStyle(.plain).foregroundStyle(.secondary)
            Button { NSApp.terminate(nil) } label: {
                Image(systemName: "power").font(.system(size: 11))
            }
            .buttonStyle(.plain).foregroundStyle(.secondary)
        }
    }

    private var dot: Color {
        if dev.ip.isEmpty { return .secondary.opacity(0.5) }
        switch dev.reachable {
        case .some(true):  return dev.paired && !dev.linked ? .orange : .green
        case .some(false): return .red
        default:           return .secondary.opacity(0.5)
        }
    }

    private var compose: some View {
        HStack(spacing: 7) {
            TextField("Say something", text: $draft)
                .textFieldStyle(.plain)
                .font(.system(size: 12))
                .focused($typing)
                .onSubmit(send)
                .padding(.horizontal, 9).padding(.vertical, 7)
                .background(RoundedRectangle(cornerRadius: 9, style: .continuous)
                    .fill(Color.primary.opacity(0.07)))
            Button(action: send) {
                Image(systemName: "arrow.up.circle.fill").font(.system(size: 17))
            }
            .buttonStyle(.plain)
            .disabled(draft.trimmingCharacters(in: .whitespaces).isEmpty || dev.busy)
            .foregroundStyle(draft.trimmingCharacters(in: .whitespaces).isEmpty
                             ? AnyShapeStyle(Color.secondary) : AnyShapeStyle(Color.accentColor))
        }
    }

    private var grid: some View {
        LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 7), count: 3),
                  spacing: 7) {

            // row one
            Tile(icon: "text.quote", name: "Phrases", detail: "saved lines") {
                showPhrases = true
            }
            Tile(icon: "timer", name: "Focus",
                 detail: dev.focusLeft > 0 ? "\(dev.focusLeft / 60 + 1) min left" : "",
                 on: dev.focusLeft > 0) {
                if dev.focusLeft > 0 { Task { await dev.stopFocus() } } else { showFocus = true }
            }
            Tile(icon: "eyes", name: "Follow", detail: "the pointer", on: dev.following) {
                Task { await dev.setFollow(!dev.following); svc.syncCursor() }
            }

            // row two
            Tile(icon: "wind", name: "Relax", detail: "screensaver", on: dev.relaxing) {
                Task { await dev.setRelax(!dev.relaxing) }
            }
            Tile(icon: "doc.on.clipboard", name: "Clipboard",
                 detail: dev.watchClipboard ? "mirroring" : "off", on: dev.watchClipboard) {
                dev.watchClipboard.toggle(); svc.syncClipboard()
            }
            Tile(icon: "figure.walk", name: "Breaks",
                 detail: dev.breakOn ? "every \(dev.breakMins)m" : "off", on: dev.breakOn) {
                dev.breakOn.toggle(); svc.syncBreaks()
            }

            // row three
            Tile(icon: "bell", name: "Remind me",
                 detail: Reminders.shared.pending.isEmpty
                       ? "nothing set" : "\(Reminders.shared.pending.count) waiting",
                 on: !Reminders.shared.pending.isEmpty) {
                showRemind = true
            }
            Tile(icon: "cup.and.saucer", name: "On a break",
                 detail: dev.dndLeft > 0 ? "\(dev.dndLeft / 60 + 1) min left" : "locks the Mac",
                 on: dev.dndLeft > 0) {
                if dev.dndLeft > 0 { Task { await dev.endBreak() } } else { showBreak = true }
            }
            Tile(icon: "video", name: "Camera & mic",
                 detail: dev.watchAV ? (svc.avLive ? "live now" : "watching") : "off",
                 on: dev.watchAV) {
                dev.watchAV.toggle(); svc.syncAV()
            }

            // row four
            Tile(icon: "arrow.down.circle", name: "Update", detail: "the robot") {
                Task { await dev.checkUpdate() }
            }
            Tile(icon: "moon.zzz", name: "Deep sleep", detail: "power to wake") {
                Task { await dev.deepSleep() }
            }
            // Wired up and tested, but deliberately inert for now.
            Tile(icon: "paintbrush.pointed", name: "Draw", detail: "not yet",
                 enabled: false) {
                Task { await dev.canvas(TestCard.bytes(), seconds: 10) }
            }
        }
    }

    // ---------------------------------------------------------------

    private func send() {
        let t = draft
        draft = ""
        Task { await dev.say(t) }
    }

}

// ===================================================================

struct Settings: View {
    @EnvironmentObject var dev: Device
    @ObservedObject var up = Updater.shared
    @Binding var showing: Bool
    @State private var addr = ""
    @State private var customOn = false

    private let breakChoices = [5, 10, 20, 30, 45, 60, 90]

    var body: some View {
        ScrollView {
          VStack(alignment: .leading, spacing: 10) {
            Text("Settings").font(.system(size: 12, weight: .semibold))

            Text("Address").font(.system(size: 10)).foregroundStyle(.secondary)
            HStack(spacing: 7) {
                TextField("192.168.1.42", text: $addr)
                    .textFieldStyle(.plain)
                    .font(.system(size: 12, design: .monospaced))
                    .padding(.horizontal, 9).padding(.vertical, 7)
                    .background(RoundedRectangle(cornerRadius: 9, style: .continuous)
                        .fill(Color.primary.opacity(0.07)))
                    .onSubmit(save)
                Button("Save", action: save).font(.system(size: 11))
            }
            Text("On the robot: SYSTEM shows it.")
                .font(.system(size: 9)).foregroundStyle(.secondary)

            Divider()

            HStack {
                VStack(alignment: .leading, spacing: 1) {
                    Text("Paired").font(.system(size: 11))
                    Text(dev.paired ? "Only this Mac can drive it"
                                    : "Anyone on your network can drive it")
                        .font(.system(size: 9)).foregroundStyle(.secondary)
                }
                Spacer()
                if dev.paired {
                    Button("Forget") { Task { await dev.unpair() } }.font(.system(size: 11))
                } else {
                    Button("Pair") { showing = false; Task { await dev.requestCode() } }
                        .font(.system(size: 11))
                }
            }

            // ---- breaks ----
            HStack {
                Text("Break every").font(.system(size: 11))
                Spacer()
                Picker("", selection: Binding(
                    get: { customOn || !breakChoices.contains(dev.breakMins) ? -1 : dev.breakMins },
                    set: { v in
                        if v == -1 { customOn = true; dev.breakMins = max(5, dev.breakCustom) }
                        else { customOn = false; dev.breakMins = v }
                    })) {
                    ForEach(breakChoices, id: \.self) { Text("\($0) min").tag($0) }
                    Text("Custom").tag(-1)
                }
                .labelsHidden().frame(width: 104)
            }
            if customOn || !breakChoices.contains(dev.breakMins) {
                HStack(spacing: 7) {
                    Stepper(value: Binding(get: { max(5, dev.breakMins) },
                                           set: { dev.breakMins = max(5, min(90, $0));
                                                  dev.breakCustom = dev.breakMins }),
                            in: 5...90, step: 5) {
                        Text("\(max(5, dev.breakMins)) minutes")
                            .font(.system(size: 11, design: .monospaced))
                    }
                    .controlSize(.mini)
                }
                Text("Five minutes is the shortest, ninety the longest.")
                    .font(.system(size: 9)).foregroundStyle(.secondary)
            }

            // ---- locking ----
            Toggle(isOn: Binding(get: { dev.lockWhenIdle }, set: { dev.lockWhenIdle = $0 })) {
                VStack(alignment: .leading, spacing: 1) {
                    Text("Lock when I walk away").font(.system(size: 11))
                    Text("After \(dev.lockIdleMins) minutes with no keyboard or mouse")
                        .font(.system(size: 9)).foregroundStyle(.secondary)
                }
            }
            .toggleStyle(.switch).controlSize(.mini)
            if dev.lockWhenIdle {
                Picker("", selection: Binding(get: { dev.lockIdleMins },
                                              set: { dev.lockIdleMins = $0 })) {
                    ForEach([2, 5, 10, 15, 30], id: \.self) { Text("\($0) min").tag($0) }
                }
                .labelsHidden().frame(width: 104)
            }

            Toggle(isOn: Binding(get: { dev.watchClipboard },
                                 set: { dev.watchClipboard = $0 })) {
                VStack(alignment: .leading, spacing: 1) {
                    Text("Send what I copy").font(.system(size: 11))
                    Text("Skips anything a password manager marks")
                        .font(.system(size: 9)).foregroundStyle(.secondary)
                }
            }
            .toggleStyle(.switch).controlSize(.mini)

            // ---- quick phrases ----
            Text("Quick phrases, one per line")
                .font(.system(size: 10)).foregroundStyle(.secondary)
            TextEditor(text: Binding(get: { dev.phrasesRaw }, set: { dev.phrasesRaw = $0 }))
                .font(.system(size: 11))
                .frame(height: 62)
                .scrollContentBackground(.hidden)
                .padding(5)
                .background(RoundedRectangle(cornerRadius: 9, style: .continuous)
                    .fill(Color.primary.opacity(0.07)))

            // ---- appearance ----
            HStack {
                Text("Appearance").font(.system(size: 11))
                Spacer()
                Picker("", selection: Binding(get: { dev.theme }, set: { dev.theme = $0 })) {
                    Text("System").tag("system")
                    Text("Light").tag("light")
                    Text("Dark").tag("dark")
                }
                .labelsHidden().frame(width: 104)
            }

            Divider()

            // ---- updating Rafiq itself ----
            HStack {
                VStack(alignment: .leading, spacing: 1) {
                    Text("Rafiq \(up.current)").font(.system(size: 11))
                    Text(updateNote).font(.system(size: 9)).foregroundStyle(.secondary)
                }
                Spacer()
                switch up.phase {
                case .checking, .downloading, .installing:
                    ProgressView().controlSize(.small)
                case .found:
                    Button("Install") { Task { await up.check(andInstall: true) } }
                        .font(.system(size: 11))
                default:
                    Button("Check") { Task { await up.check(andInstall: false) } }
                        .font(.system(size: 11))
                }
            }

            HStack {
                Text(dev.version.isEmpty ? "" : "robot \(dev.version)")
                    .font(.system(size: 9)).foregroundStyle(.secondary)
                Spacer()
                Button("Done") { showing = false }.font(.system(size: 11))
            }
          }
        }
        .frame(maxHeight: 440)
        .onAppear { addr = dev.ip; customOn = !breakChoices.contains(dev.breakMins) }
    }

    private var updateNote: String {
        switch up.phase {
        case .idle:        return "Check for a newer version"
        case .checking:    return "Looking..."
        case .none:        return "You are up to date"
        case .found(let v): return "Version \(v) is available"
        case .downloading: return "Downloading..."
        case .installing:  return "Installing, it will restart"
        case .failed(let m): return m
        }
    }

    private func save() {
        dev.ip = addr.trimmingCharacters(in: .whitespaces)
        Task { await dev.refresh() }
        if !dev.ip.isEmpty { showing = false }
    }
}

// ===================================================================
//  One screenful of pixels, to prove the canvas works end to end.
//  128 by 64, one bit each, top row first, which is the layout the
//  SSD1306 buffer already uses.
// ===================================================================
enum TestCard {
    static func bytes() -> Data {
        var buf = [UInt8](repeating: 0, count: 1024)
        func plot(_ x: Int, _ y: Int) {
            guard (0..<128).contains(x), (0..<64).contains(y) else { return }
            buf[x + (y >> 3) * 128] |= UInt8(1 << (y & 7))
        }
        // a frame, and a heart in the middle of it
        for x in 0..<128 { plot(x, 0); plot(x, 63) }
        for y in 0..<64  { plot(0, y); plot(127, y) }
        for t in stride(from: 0.0, through: 6.2832, by: 0.004) {
            let s = 2.1
            let hx = 16 * pow(sin(t), 3)
            let hy = 13 * cos(t) - 5 * cos(2*t) - 2 * cos(3*t) - cos(4*t)
            plot(64 + Int(hx * s / 2.4), 32 - Int(hy * s / 2.4))
        }
        return Data(buf)
    }
}
