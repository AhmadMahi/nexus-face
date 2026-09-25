import SwiftUI
import AppKit

/// Everything that has to keep running whether or not the panel is open.
///
/// This lived in the panel's `onAppear` at first, which meant the menu bar
/// icon showed red until you clicked it and the clipboard was never mirrored
/// unless you had opened the window at least once. None of that is work the
/// view should own.
@MainActor
final class Services: ObservableObject {
    static let shared = Services(Device.shared)

    private let dev: Device
    private let clip = Clipboard()
    private let cursor = Cursor()
    private let activity = Activity()
    private let av = AVWatch()

    /// True while the built-in camera or microphone is actually live.
    @Published var avLive = false
    private var lockedForIdle = false

    private var poll: Timer?
    private var idle: Timer?
    private var breaks: Timer?
    private var lastNudge = Date.distantPast

    init(_ dev: Device) { self.dev = dev }

    func start() {
        Task { await dev.refresh() }
        idle?.invalidate()
        // Locking when you have walked away is worth checking often enough
        // to be useful and rarely enough to cost nothing.
        idle = Timer.scheduledTimer(withTimeInterval: 20, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.checkIdle() }
        }
        poll?.invalidate()
        // Ten seconds keeps the menu bar honest without being chatty. The
        // device answers this in well under a millisecond.
        poll = Timer.scheduledTimer(withTimeInterval: 10, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.dev.refresh() }
        }
        syncClipboard()
        syncBreaks()
        syncCursor()
        syncAV()
        startReminders()
    }

    /// Reminders live on the Mac and simply say the word when due.
    private func startReminders() {
        Reminders.shared.start { [weak self] r in
            Task { @MainActor in await self?.dev.remind(r.text) }
        }
    }

    func syncAV() {
        guard dev.watchAV else {
            av.stop()
            avLive = false
            Task { await dev.setBusy(cam: false, mic: false) }
            return
        }
        av.start { [weak self] cam, mic in
            Task { @MainActor in
                guard let self else { return }
                self.avLive = cam || mic
                await self.dev.setBusy(cam: cam, mic: mic)
            }
        }
    }

    func syncClipboard() {
        guard dev.watchClipboard else { clip.stop(); return }
        clip.start { [weak self] text in
            Task { @MainActor in await self?.dev.toast(text, kind: "copy") }
        }
    }

    func syncCursor() {
        if dev.following && !dev.ip.isEmpty {
            cursor.start(host: Self.host(dev.ip))
        } else {
            cursor.stop()
        }
    }

    /// The address may carry a port for testing; the pointer always goes to
    /// the fixed UDP port, so only the host part of it is useful here.
    static func host(_ s: String) -> String {
        let t = s.trimmingCharacters(in: .whitespaces)
        return t.split(separator: ":").first.map(String.init) ?? t
    }

    func syncBreaks() {
        breaks?.invalidate(); breaks = nil
        guard dev.breakOn else { activity.stop(); return }
        activity.start { _ in }
        breaks = Timer.scheduledTimer(withTimeInterval: 60, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self, self.dev.breakOn else { return }
                guard self.activity.due(after: self.dev.breakMins) else { return }
                // Even if you ignore it, it does not come straight back.
                guard Date().timeIntervalSince(self.lastNudge) > 600 else { return }
                self.lastNudge = Date()
                await self.dev.toast("You have been at it \(self.dev.breakMins) minutes",
                                     kind: "break", seconds: 20)
                self.activity.reset()
            }
        }
    }

    /// Away long enough that the desk should not be left open. It locks
    /// once per absence, not every time this runs, so coming back and
    /// stepping away again is what arms it afresh.
    private func checkIdle() {
        guard dev.lockWhenIdle else { lockedForIdle = false; return }
        let secs = CGEventSource.secondsSinceLastEventType(.combinedSessionState,
                                                           eventType: .init(rawValue: ~0)!)
        if secs < 30 { lockedForIdle = false; return }
        guard !lockedForIdle, secs >= Double(max(1, dev.lockIdleMins)) * 60 else { return }
        lockedForIdle = true
        Screen.lock()
    }

    var workedMinutes: Int { activity.workedMinutes }
}
