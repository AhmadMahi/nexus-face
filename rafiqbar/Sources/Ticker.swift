import Foundation

extension Timer {
    /// Repeating work that survives the panel being open.
    ///
    /// `Timer.scheduledTimer` puts a timer in the default run loop mode
    /// only. The moment a menu bar window opens, the run loop switches to
    /// event tracking and every one of those timers stops until it closes
    /// again. That killed the pointer stream exactly while you were looking
    /// at the panel: no packets went out, the robot's tracking lapsed after
    /// a couple of seconds, and the eyes went back to normal on their own.
    ///
    /// Adding to `.common` covers both modes, so these keep running whether
    /// or not the panel is up.
    @discardableResult
    static func every(_ seconds: TimeInterval, _ body: @escaping () -> Void) -> Timer {
        let t = Timer(timeInterval: seconds, repeats: true) { _ in body() }
        RunLoop.main.add(t, forMode: .common)
        return t
    }
}
