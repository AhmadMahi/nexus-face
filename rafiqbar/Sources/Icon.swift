import AppKit

/// The menu bar face: a small robot head whose eyes carry the state.
///
/// Drawn rather than shipped as artwork, because the eyes have to change
/// colour and a template image would have the system flatten them to one
/// tone. The head itself is drawn in the label colour so it still follows
/// a light or dark menu bar.
enum RobotIcon {
    enum State { case linked, adrift, unset }

    static func image(_ state: State) -> NSImage {
        let size = NSSize(width: 18, height: 18)
        let img = NSImage(size: size, flipped: false) { _ in
            let head = NSColor.labelColor
            let eye: NSColor
            switch state {
            case .linked: eye = NSColor.systemGreen
            case .adrift: eye = NSColor.systemRed
            case .unset:  eye = NSColor.tertiaryLabelColor
            }

            // aerial
            head.setStroke()
            let ant = NSBezierPath()
            ant.move(to: NSPoint(x: 9, y: 14.2))
            ant.line(to: NSPoint(x: 9, y: 16))
            ant.lineWidth = 1.2
            ant.stroke()
            head.setFill()
            NSBezierPath(ovalIn: NSRect(x: 7.9, y: 15.4, width: 2.2, height: 2.2)).fill()

            // head
            let box = NSBezierPath(roundedRect: NSRect(x: 2.6, y: 3.2, width: 12.8, height: 11),
                                   xRadius: 3.4, yRadius: 3.4)
            box.lineWidth = 1.4
            box.stroke()

            // ears
            let ears = NSBezierPath()
            ears.move(to: NSPoint(x: 1.2, y: 9)); ears.line(to: NSPoint(x: 2.6, y: 9))
            ears.move(to: NSPoint(x: 15.4, y: 9)); ears.line(to: NSPoint(x: 16.8, y: 9))
            ears.lineWidth = 1.4
            ears.stroke()

            // eyes, which are the whole point of it
            eye.setFill()
            NSBezierPath(ovalIn: NSRect(x: 5.1, y: 8.1, width: 2.9, height: 2.9)).fill()
            NSBezierPath(ovalIn: NSRect(x: 10.0, y: 8.1, width: 2.9, height: 2.9)).fill()

            // mouth
            head.setStroke()
            let m = NSBezierPath()
            m.move(to: NSPoint(x: 6.4, y: 5.7)); m.line(to: NSPoint(x: 11.6, y: 5.7))
            m.lineWidth = 1.2
            m.stroke()
            return true
        }
        // Not a template: the system would drop the eye colour if it were.
        img.isTemplate = false
        return img
    }
}
