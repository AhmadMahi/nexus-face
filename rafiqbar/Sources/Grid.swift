import SwiftUI

/// One of the nine. A tile is lit when the robot says the thing is on, not
/// when we asked for it, so the grid can never disagree with the desk.
struct Tile: View {
    let icon: String
    let name: String
    var detail: String = ""
    var on: Bool = false
    var enabled: Bool = true
    let act: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: act) {
            VStack(spacing: 5) {
                Image(systemName: icon)
                    .font(.system(size: 17, weight: .regular))
                    .frame(height: 20)
                Text(name)
                    .font(.system(size: 10, weight: .medium))
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
                Text(detail)
                    .font(.system(size: 9))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .opacity(detail.isEmpty ? 0 : 1)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 9)
            .background(
                RoundedRectangle(cornerRadius: 11, style: .continuous)
                    .fill(on ? AnyShapeStyle(Color.accentColor.opacity(0.22))
                             : AnyShapeStyle(Color.primary.opacity(hovering ? 0.10 : 0.05)))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 11, style: .continuous)
                    .strokeBorder(on ? Color.accentColor.opacity(0.55) : .clear, lineWidth: 1)
            )
            .foregroundStyle(on ? AnyShapeStyle(Color.accentColor) : AnyShapeStyle(Color.primary))
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.35)
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.12), value: hovering)
        .animation(.easeOut(duration: 0.18), value: on)
    }
}

/// How long to focus for. Shown in place of the grid rather than over it,
/// because a sheet inside a menu bar window is a fight with the system.
struct FocusPicker: View {
    @EnvironmentObject var dev: Device
    @Binding var showing: Bool

    private let choices = [5, 10, 15, 25, 30, 45, 60, 90]

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Focus for").font(.system(size: 12, weight: .semibold))
                Spacer()
                Button { showing = false } label: {
                    Image(systemName: "xmark").font(.system(size: 10, weight: .bold))
                }
                .buttonStyle(.plain).foregroundStyle(.secondary)
            }
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 6), count: 4),
                      spacing: 6) {
                ForEach(choices, id: \.self) { m in
                    Button {
                        showing = false
                        Task { await dev.startFocus(m) }
                    } label: {
                        Text("\(m)")
                            .font(.system(size: 13, weight: .medium))
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 8)
                            .background(RoundedRectangle(cornerRadius: 9, style: .continuous)
                                .fill(Color.primary.opacity(0.07)))
                    }
                    .buttonStyle(.plain)
                }
            }
            Text("The panel shows the countdown, then rests, then shows it again. "
                 + "It will not drop off until the time is up.")
                .font(.system(size: 10))
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}

/// Six digits, read off the robot's own panel. Nothing else on the network
/// can see them, which is what makes this worth doing at all.
struct PairView: View {
    @EnvironmentObject var dev: Device
    @State private var code = ""
    @FocusState private var focused: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 9) {
            Text("Pair with the robot").font(.system(size: 12, weight: .semibold))
            Text("Six digits are on its panel now. They last three minutes.")
                .font(.system(size: 10)).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            TextField("000000", text: $code)
                .textFieldStyle(.plain)
                .font(.system(size: 22, weight: .medium, design: .monospaced))
                .multilineTextAlignment(.center)
                .padding(.vertical, 8)
                .background(RoundedRectangle(cornerRadius: 9, style: .continuous)
                    .fill(Color.primary.opacity(0.07)))
                .focused($focused)
                .onSubmit { Task { await dev.pair(with: code) } }
                .onChange(of: code) { _, v in
                    let d = String(v.filter(\.isNumber).prefix(6))
                    if d != v { code = d }
                    if d.count == 6 { Task { await dev.pair(with: d) } }
                }
            if !dev.pairError.isEmpty {
                Text(dev.pairError).font(.system(size: 10)).foregroundStyle(.red)
            }
            HStack {
                Button("New code") { Task { await dev.requestCode() } }
                Spacer()
                Button("Cancel") { dev.pairing = false }
            }
            .font(.system(size: 11))
            .buttonStyle(.plain)
            .foregroundStyle(.secondary)
        }
        .onAppear { focused = true }
    }
}

// ===================================================================
//  The sheets the tiles open. All of them replace the grid rather than
//  sitting over it: a sheet inside a menu bar window is a fight with
//  the system, and this keeps one thing on screen at a time.
// ===================================================================

/// A short list of things you send often.
struct Phrases: View {
    @EnvironmentObject var dev: Device
    @Binding var showing: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            SheetHead(title: "Quick phrases", showing: $showing)
            if dev.phrases.isEmpty {
                Text("Add some in settings, one per line.")
                    .font(.system(size: 10)).foregroundStyle(.secondary)
            }
            ForEach(dev.phrases, id: \.self) { p in
                Button {
                    showing = false
                    Task { await dev.say(p) }
                } label: {
                    HStack {
                        Text(p).font(.system(size: 12)).lineLimit(1)
                        Spacer()
                        Image(systemName: "arrow.up.right").font(.system(size: 9))
                            .foregroundStyle(.secondary)
                    }
                    .padding(.horizontal, 9).padding(.vertical, 7)
                    .background(RoundedRectangle(cornerRadius: 9, style: .continuous)
                        .fill(Color.primary.opacity(0.06)))
                }
                .buttonStyle(.plain)
            }
        }
    }
}

/// How long, for focus or for a break. One grid of choices, reused.
struct Minutes: View {
    let title: String
    let choices: [Int]
    let note: String
    @Binding var showing: Bool
    let pick: (Int) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            SheetHead(title: title, showing: $showing)
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 6), count: 4),
                      spacing: 6) {
                ForEach(choices, id: \.self) { m in
                    Button {
                        showing = false
                        pick(m)
                    } label: {
                        Text(m >= 60 && m % 60 == 0 ? "\(m / 60)h" : "\(m)")
                            .font(.system(size: 13, weight: .medium))
                            .frame(maxWidth: .infinity).padding(.vertical, 8)
                            .background(RoundedRectangle(cornerRadius: 9, style: .continuous)
                                .fill(Color.primary.opacity(0.07)))
                    }
                    .buttonStyle(.plain)
                }
            }
            Text(note).font(.system(size: 10)).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}

/// What to be reminded about, and when.
struct RemindSheet: View {
    @EnvironmentObject var dev: Device
    @ObservedObject var store = Reminders.shared
    @Binding var showing: Bool

    @State private var text = ""
    @State private var mins = 15
    @State private var mode = 0                 // 0 in a while, 1 at a time
    @State private var hour = Calendar.current.component(.hour, from: Date().addingTimeInterval(3600))
    @State private var minute = 0
    @FocusState private var typing: Bool

    private let quick = [5, 10, 15, 30, 45, 60, 90, 120]

    var body: some View {
        VStack(alignment: .leading, spacing: 9) {
            SheetHead(title: "Remind me", showing: $showing)

            TextField("What about?", text: $text)
                .textFieldStyle(.plain)
                .font(.system(size: 12))
                .focused($typing)
                .padding(.horizontal, 9).padding(.vertical, 7)
                .background(RoundedRectangle(cornerRadius: 9, style: .continuous)
                    .fill(Color.primary.opacity(0.07)))
                .onSubmit(add)

            Picker("", selection: $mode) {
                Text("In").tag(0)
                Text("At").tag(1)
            }
            .pickerStyle(.segmented)
            .labelsHidden()

            if mode == 0 {
                LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 5), count: 4),
                          spacing: 5) {
                    ForEach(quick, id: \.self) { m in
                        Button { mins = m } label: {
                            Text(m >= 60 && m % 60 == 0 ? "\(m / 60)h" : "\(m)m")
                                .font(.system(size: 11, weight: mins == m ? .semibold : .regular))
                                .frame(maxWidth: .infinity).padding(.vertical, 6)
                                .background(RoundedRectangle(cornerRadius: 7, style: .continuous)
                                    .fill(mins == m ? AnyShapeStyle(Color.accentColor.opacity(0.25))
                                                    : AnyShapeStyle(Color.primary.opacity(0.06))))
                                .foregroundStyle(mins == m ? AnyShapeStyle(Color.accentColor)
                                                           : AnyShapeStyle(Color.primary))
                        }
                        .buttonStyle(.plain)
                    }
                }
            } else {
                // A DatePicker field cannot be typed into inside a menu bar
                // window: it never takes keyboard focus, so the time could
                // be seen but never set. Two plain menus always work, and
                // are quicker than typing a time anyway.
                HStack(spacing: 6) {
                    Picker("", selection: $hour) {
                        ForEach(0..<24, id: \.self) { h in
                            Text(String(format: "%02d", h)).tag(h)
                        }
                    }
                    .labelsHidden().frame(width: 70)
                    Text(":").foregroundStyle(.secondary)
                    Picker("", selection: $minute) {
                        ForEach(Array(stride(from: 0, to: 60, by: 5)), id: \.self) { m in
                            Text(String(format: "%02d", m)).tag(m)
                        }
                    }
                    .labelsHidden().frame(width: 70)
                    Spacer()
                    Text(atNote).font(.system(size: 10)).foregroundStyle(.secondary)
                }
            }

            Button(action: add) {
                Text("Add reminder")
                    .font(.system(size: 12, weight: .medium))
                    .frame(maxWidth: .infinity).padding(.vertical, 7)
                    .background(RoundedRectangle(cornerRadius: 9, style: .continuous)
                        .fill(Color.accentColor.opacity(0.22)))
                    .foregroundStyle(Color.accentColor)
            }
            .buttonStyle(.plain)
            .disabled(text.trimmingCharacters(in: .whitespaces).isEmpty)
            .opacity(text.trimmingCharacters(in: .whitespaces).isEmpty ? 0.4 : 1)

            if !store.pending.isEmpty {
                Divider().padding(.vertical, 1)
                Text("Waiting").font(.system(size: 10)).foregroundStyle(.secondary)
                ForEach(store.pending) { r in
                    HStack(spacing: 6) {
                        Text(Self.stamp(r.fireAt))
                            .font(.system(size: 10, design: .monospaced))
                            .foregroundStyle(.secondary)
                            .frame(width: 44, alignment: .leading)
                        Text(r.text).font(.system(size: 11)).lineLimit(1)
                        Spacer()
                        Button { store.remove(r) } label: {
                            Image(systemName: "xmark").font(.system(size: 8, weight: .bold))
                        }
                        .buttonStyle(.plain).foregroundStyle(.secondary)
                    }
                }
            }
        }
        .onAppear { typing = true }
    }

    private func add() {
        let t = text.trimmingCharacters(in: .whitespaces)
        guard !t.isEmpty else { return }
        if mode == 0 {
            Reminders.shared.add(t, inMinutes: mins)
        } else {
            Reminders.shared.add(t, at: nextOccurrence)
        }
        text = ""
        dev.flash("Reminder set")
    }

    /// The next time today's clock shows this. A time already gone means
    /// tomorrow, which is what anyone setting 7am at midnight expects.
    private var nextOccurrence: Date {
        let cal = Calendar.current
        var c = cal.dateComponents([.year, .month, .day], from: Date())
        c.hour = hour; c.minute = minute; c.second = 0
        var target = cal.date(from: c) ?? Date().addingTimeInterval(3600)
        if target <= Date() { target = target.addingTimeInterval(86400) }
        return target
    }

    private var atNote: String {
        Calendar.current.isDateInToday(nextOccurrence) ? "today" : "tomorrow"
    }

    static func stamp(_ d: Date) -> String {
        let f = DateFormatter()
        f.dateFormat = Calendar.current.isDateInToday(d) ? "HH:mm" : "E HH:mm"
        return f.string(from: d)
    }
}

struct SheetHead: View {
    let title: String
    @Binding var showing: Bool
    var body: some View {
        HStack {
            Text(title).font(.system(size: 12, weight: .semibold))
            Spacer()
            Button { showing = false } label: {
                Image(systemName: "xmark").font(.system(size: 10, weight: .bold))
            }
            .buttonStyle(.plain).foregroundStyle(.secondary)
        }
    }
}
