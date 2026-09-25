import Foundation
import SwiftUI
import Network

/// Everything that talks to the robot. It serves plain HTTP on the local
/// network, which is why the bundle asks for local network access and allows
/// local loads only: nothing here should ever leave the house.
@MainActor
final class Device: ObservableObject {
    static let shared = Device()

    @AppStorage("deviceIP")    var ip: String = ""
    @AppStorage("watchClip")   var watchClipboard: Bool = false
    @AppStorage("breakMins")   var breakMins: Int = 45
    @AppStorage("breakOn")     var breakOn: Bool = false
    @AppStorage("focusMins")   var focusMins: Int = 25
    @AppStorage("breakCustom") var breakCustom: Int = 25
    @AppStorage("lockIdle")    var lockWhenIdle: Bool = false
    @AppStorage("lockIdleMin") var lockIdleMins: Int = 5
    @AppStorage("watchAV")     var watchAV: Bool = false
    @AppStorage("theme")       var theme: String = "system"
    @AppStorage("phrases")     var phrasesRaw: String =
        "On my way\nBack in 5\nIn a meeting\nCall me\nDone"

    var phrases: [String] {
        phrasesRaw.split(separator: "\n").map(String.init)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
    }

    var colorScheme: ColorScheme? {
        switch theme {
        case "light": return .light
        case "dark":  return .dark
        default:      return nil
        }
    }

    @Published var status: String = ""
    @Published var reachable: Bool? = nil
    @Published var busy = false

    /// What the robot last told us about itself. Everything the grid shows
    /// comes from here, so a tile is never lit unless the device agrees.
    @Published var paired = false
    @Published var linked = false
    @Published var following = false
    @Published var relaxing = false
    @Published var focusLeft = 0
    @Published var dndLeft = 0
    @Published var version = ""

    /// Set while a six digit code is on the robot's panel.
    @Published var pairing = false
    @Published var pairError = ""

    var token: String {
        get { Keychain.get("token") }
        set { Keychain.set(newValue, for: "token") }
    }

    // ---------------------------------------------------------------
    //  text
    // ---------------------------------------------------------------

    /// The firmware keeps 84 bytes and cuts on a byte boundary, which would
    /// split a multi byte character in half. Trim by bytes here so it never
    /// has to. The panel draws ASCII, so anything fancier arrives intact but
    /// shows as blanks.
    static let maxLen = 84
    static func clip(_ s: String) -> String {
        if s.utf8.count <= maxLen { return s }
        var out = ""
        var n = 0
        for ch in s {
            let c = String(ch).utf8.count
            if n + c > maxLen - 3 { break }        // 3 bytes for the ellipsis
            out.append(ch); n += c
        }
        return out + "\u{2026}"
    }

    // ---------------------------------------------------------------
    //  what the grid does
    // ---------------------------------------------------------------

    func say(_ raw: String) async {
        let text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        await run("/api/msg", ["m": Self.clip(text.replacingOccurrences(of: "\n", with: " "))],
                  say: "Sent")
    }

    /// A copy, a paste, or a word about standing up. None of it disturbs the
    /// message the robot is holding, which is yours.
    func toast(_ text: String, kind: String, seconds: Int = 4) async {
        await run("/api/toast",
                  ["m": Self.clip(text), "k": kind, "s": String(seconds)], say: nil)
    }

    func startFocus(_ mins: Int) async {
        await run("/api/focus", ["m": String(mins)], say: "Focus for \(mins) min")
        focusLeft = mins * 60
    }
    func stopFocus() async {
        await run("/api/focus", ["m": "0"], say: "Focus stopped")
        focusLeft = 0
    }

    func setRelax(_ on: Bool) async {
        await run("/api/relax", ["a": on ? "1" : "0"], say: on ? "Resting" : "Back to normal")
        relaxing = on
    }

    func setFollow(_ on: Bool) async {
        await run("/api/follow", ["a": on ? "1" : "0"], say: on ? "Watching the pointer" : "Eyes off")
        following = on
    }

    /// On a break: the robot holds the sign, the Mac locks itself.
    func startBreak(_ mins: Int) async {
        await run("/api/dnd", ["m": String(mins)], say: "On a break for \(mins) min")
        dndLeft = mins * 60
    }
    func endBreak() async {
        await run("/api/dnd", ["m": "0"], say: "Back")
        dndLeft = 0
    }

    /// One flag per device. Nothing about what is being said or seen.
    func setBusy(cam: Bool, mic: Bool) async {
        await run("/api/busy", ["cam": cam ? "1" : "0", "mic": mic ? "1" : "0"], say: nil)
    }

    func remind(_ text: String) async {
        await toast(text, kind: "remind", seconds: 25)
    }

    func checkUpdate() async {
        await run("/api/update", [:], say: "Looking for an update")
    }

    /// Nothing wakes it from this but the power switch, which is what was
    /// asked for, so the panel is what confirms it rather than this app.
    func deepSleep() async {
        await run("/api/deepsleep", [:], say: "Going to sleep")
        linked = false
    }

    /// One screenful of pixels, 128 by 64, a bit each, top row first.
    func canvas(_ bytes: Data, seconds: Int = 8) async {
        guard bytes.count == 1024 else { flash("A screen is 1024 bytes"); return }
        await run("/api/canvas", ["b": bytes.base64EncodedString(), "s": String(seconds)], say: "Drawn")
    }

    // ---------------------------------------------------------------
    //  pairing
    // ---------------------------------------------------------------

    /// Asking for a code needs no token, otherwise a device whose token you
    /// had lost could never be paired again. Reading the code still means
    /// standing in front of the thing, which is the whole point of it.
    func requestCode() async {
        guard !ip.isEmpty else { flash("Set the address first"); return }
        pairError = ""
        do {
            try await post("/api/paircode", [:])
            pairing = true
        } catch {
            pairError = "Could not reach it"
        }
    }

    func pair(with code: String) async {
        let digits = code.filter(\.isNumber)
        guard digits.count == 6 else { pairError = "Six digits"; return }
        do {
            let body = try await post("/api/pair", ["c": digits])
            guard let t = Self.jsonString(body, "token"), !t.isEmpty else {
                pairError = "Wrong code"; return
            }
            token = t
            paired = true
            pairing = false
            pairError = ""
            flash("Paired")
            await refresh()
        } catch {
            pairError = "Wrong code, or it expired"
        }
    }

    func unpair() async {
        await run("/api/unpair", [:], say: "Unpaired")
        token = ""
        paired = false
    }

    // ---------------------------------------------------------------
    //  state
    // ---------------------------------------------------------------

    func refresh() async {
        guard !ip.isEmpty else { reachable = nil; return }
        do {
            var req = URLRequest(url: try url("/api/state"))
            req.timeoutInterval = 3
            req.setValue("1", forHTTPHeaderField: "X-Rafiq-App")
            if !token.isEmpty { req.setValue(token, forHTTPHeaderField: "X-Rafiq-Token") }
            let (data, resp) = try await URLSession.shared.data(for: req)
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            // 401 still means the robot answered, so the dot stays green and
            // the panel shows the pairing prompt instead of a dead device.
            reachable = (code == 200 || code == 401)
            guard code == 200, let s = String(data: data, encoding: .utf8) else {
                if code == 401 { paired = true; linked = false }
                return
            }
            paired    = Self.jsonBool(s, "paired")
            linked    = Self.jsonBool(s, "linked")
            following = Self.jsonBool(s, "follow")
            relaxing  = Self.jsonBool(s, "relax")
            focusLeft = Self.jsonInt(s, "focusLeft")
            dndLeft   = Self.jsonInt(s, "dndLeft")
            version   = Self.jsonString(s, "fw") ?? version
        } catch {
            reachable = false
            linked = false
        }
    }

    // ---------------------------------------------------------------
    //  transport
    // ---------------------------------------------------------------

    private var statusClear: Task<Void, Never>?

    private func run(_ path: String, _ fields: [String: String], say: String?) async {
        guard !ip.isEmpty else { flash("Set the address first"); return }
        busy = true
        defer { busy = false }
        do {
            _ = try await post(path, fields)
            reachable = true
            if let say { flash(say) }
        } catch let e as URLError where e.code == .userAuthenticationRequired {
            reachable = true
            flash("Pair with the robot first")
        } catch {
            reachable = false
            flash("Could not reach it")
        }
    }

    private func url(_ path: String) throws -> URL {
        let host = ip.trimmingCharacters(in: .whitespaces)
        guard let u = URL(string: "http://\(host)\(path)") else { throw URLError(.badURL) }
        return u
    }

    @discardableResult
    private func post(_ path: String, _ fields: [String: String]) async throws -> String {
        var req = URLRequest(url: try url(path))
        req.httpMethod = "POST"
        req.timeoutInterval = 6
        req.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        req.setValue("1", forHTTPHeaderField: "X-Rafiq-App")
        if !token.isEmpty { req.setValue(token, forHTTPHeaderField: "X-Rafiq-Token") }
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._~")
        req.httpBody = fields
            .map { k, v in "\(k)=\(v.addingPercentEncoding(withAllowedCharacters: allowed) ?? "")" }
            .joined(separator: "&")
            .data(using: .utf8)
        let (data, resp) = try await URLSession.shared.data(for: req)
        let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
        if code == 401 || code == 403 { throw URLError(.userAuthenticationRequired) }
        guard code == 200 else { throw URLError(.badServerResponse) }
        return String(data: data, encoding: .utf8) ?? ""
    }

    func flash(_ s: String) {
        status = s
        statusClear?.cancel()
        statusClear = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 2_200_000_000)
            guard !Task.isCancelled else { return }
            await MainActor.run { self?.status = "" }
        }
    }

    // ---------------------------------------------------------------
    //  Small readers rather than a decoder: the device hand rolls its
    //  JSON and one unexpected field should never cost us the rest.
    // ---------------------------------------------------------------

    static func jsonString(_ b: String, _ key: String) -> String? {
        guard let r = b.range(of: "\"\(key)\"") else { return nil }
        var i = r.upperBound
        while i < b.endIndex, b[i] == ":" || b[i] == " " { i = b.index(after: i) }
        guard i < b.endIndex, b[i] == "\"" else { return nil }
        i = b.index(after: i)
        guard let end = b[i...].firstIndex(of: "\"") else { return nil }
        return String(b[i..<end])
    }
    static func jsonRaw(_ b: String, _ key: String) -> String? {
        guard let r = b.range(of: "\"\(key)\"") else { return nil }
        var i = r.upperBound
        while i < b.endIndex, b[i] == ":" || b[i] == " " { i = b.index(after: i) }
        var out = ""
        while i < b.endIndex, b[i] != ",", b[i] != "}" { out.append(b[i]); i = b.index(after: i) }
        return out.trimmingCharacters(in: .whitespaces)
    }
    static func jsonBool(_ b: String, _ key: String) -> Bool { jsonRaw(b, key) == "true" }
    static func jsonInt(_ b: String, _ key: String) -> Int { Int(jsonRaw(b, key) ?? "") ?? 0 }
}

// ===================================================================
//  The pointer, sent over UDP
// ===================================================================
/// Ten a second through a fresh HTTP handshake each time would drown the
/// device, and a lost reading costs nothing because another is a tenth of a
/// second behind it. So these go out as bare datagrams and nothing is
/// acknowledged or retried.
@MainActor
final class Cursor {
    private var conn: NWConnection?
    private var timer: Timer?
    private var host = ""

    func start(host: String) {
        stop()
        self.host = host
        guard let p = NWEndpoint.Port(rawValue: 4210) else { return }
        let c = NWConnection(host: NWEndpoint.Host(host), port: p, using: .udp)
        c.start(queue: .global(qos: .utility))
        conn = c
        timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
    }

    func stop() {
        timer?.invalidate(); timer = nil
        conn?.cancel(); conn = nil
    }

    private func tick() {
        guard let conn else { return }
        let p = NSEvent.mouseLocation
        // The screen the pointer is actually on, so a second monitor does
        // not send the eyes hard over to one side and leave them there.
        let screen = NSScreen.screens.first { $0.frame.contains(p) } ?? NSScreen.main
        guard let f = screen?.frame, f.width > 0, f.height > 0 else { return }
        let nx = Float((p.x - f.minX) / f.width) * 2 - 1
        // AppKit measures up from the bottom and the robot looks down from
        // the top of the monitor, so this is flipped.
        let ny = 1 - Float((p.y - f.minY) / f.height) * 2
        let msg = "\(Int(nx * 1000)) \(Int(ny * 1000))"
        conn.send(content: msg.data(using: .utf8), completion: .idempotent)
    }
}
