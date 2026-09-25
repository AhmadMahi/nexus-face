import Foundation
import AppKit

/// Watches how long you have been at the machine without a break, and asks
/// the robot to say something when it has been too long.
///
/// This reads one number from the system: how many seconds since the last
/// input event of any kind. It does not see what you typed, what you clicked
/// or what you had open, and it could not if it wanted to.
@MainActor
final class Activity: ObservableObject {
    /// Long enough away from the keyboard to count as a real break.
    private let restSeconds: Double = 180
    /// Beyond this, you left the room rather than stopped for a moment, so
    /// the count is not advanced by time you were not there.
    private let awaySeconds: Double = 60

    @Published private(set) var workedMinutes = 0

    private var timer: Timer?
    private var last = Date()
    private var worked: Double = 0
    private var onDue: ((Int) -> Void)?

    func start(onDue: @escaping (Int) -> Void) {
        self.onDue = onDue
        stop()
        last = Date()
        timer = Timer.scheduledTimer(withTimeInterval: 20, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
    }

    func stop() { timer?.invalidate(); timer = nil }

    /// Called when you take the break, or snooze it.
    func reset(keeping minutes: Double = 0) {
        worked = minutes * 60
        workedMinutes = Int(worked / 60)
    }

    private func tick() {
        let idle = CGEventSource.secondsSinceLastEventType(.combinedSessionState,
                                                           eventType: .init(rawValue: ~0)!)
        let gap = Date().timeIntervalSince(last)
        last = Date()

        if idle >= restSeconds {
            // Away long enough that it counts. Start again from nothing.
            if worked > 0 { worked = 0; workedMinutes = 0 }
            return
        }
        // Time while the lid was shut or the machine was asleep is not time
        // spent working, so only credit a gap we were plausibly present for.
        worked += min(gap, awaySeconds)
        workedMinutes = Int(worked / 60)
    }

    /// True when it is time to say something, given the interval in settings.
    func due(after minutes: Int) -> Bool { worked >= Double(minutes) * 60 }
}
