import Foundation
import SwiftUI

struct Reminder: Codable, Identifiable, Equatable {
    var id = UUID()
    var text: String
    var fireAt: Date
    var done = false
}

/// A short list of things to be told about, held on the Mac rather than on
/// the robot. The robot sleeps and loses track of time; the Mac does not,
/// so it keeps the list and simply says the word when one comes due.
@MainActor
final class Reminders: ObservableObject {
    static let shared = Reminders()
    static let maxKept = 20

    @Published private(set) var items: [Reminder] = []

    private var timer: Timer?
    private var fire: ((Reminder) -> Void)?
    private let key = "reminders"

    init() { load() }

    var pending: [Reminder] {
        items.filter { !$0.done }.sorted { $0.fireAt < $1.fireAt }
    }

    func start(_ fire: @escaping (Reminder) -> Void) {
        self.fire = fire
        timer?.invalidate()
        // Ten seconds is close enough for something measured in minutes,
        // and it costs nothing to check.
        timer = Timer.scheduledTimer(withTimeInterval: 10, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
        tick()
    }

    func add(_ text: String, at when: Date) {
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !t.isEmpty else { return }
        items.append(Reminder(text: t, fireAt: when))
        trim()
        save()
    }

    func add(_ text: String, inMinutes m: Int) {
        add(text, at: Date().addingTimeInterval(TimeInterval(max(1, m) * 60)))
    }

    func remove(_ r: Reminder) {
        items.removeAll { $0.id == r.id }
        save()
    }

    /// Ten minutes on, the same as a snooze on the robot.
    func snooze(_ r: Reminder) {
        guard let i = items.firstIndex(where: { $0.id == r.id }) else { return }
        items[i].done = false
        items[i].fireAt = Date().addingTimeInterval(600)
        save()
    }

    private func tick() {
        let now = Date()
        for i in items.indices where !items[i].done && items[i].fireAt <= now {
            items[i].done = true
            fire?(items[i])
        }
        // Anything long since said is cleared out, so the list stays the
        // things still ahead of you rather than a history.
        let cutoff = now.addingTimeInterval(-3600)
        items.removeAll { $0.done && $0.fireAt < cutoff }
        save()
    }

    private func trim() {
        let extra = pending.count - Self.maxKept
        if extra > 0 {
            for r in pending.suffix(extra) { items.removeAll { $0.id == r.id } }
        }
    }

    private func save() {
        guard let d = try? JSONEncoder().encode(items) else { return }
        UserDefaults.standard.set(d, forKey: key)
    }
    private func load() {
        guard let d = UserDefaults.standard.data(forKey: key),
              let v = try? JSONDecoder().decode([Reminder].self, from: d) else { return }
        items = v
    }
}
