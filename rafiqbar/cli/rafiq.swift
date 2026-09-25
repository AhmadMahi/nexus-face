import Foundation

// rafiq: a one line way for anything on this Mac to reach the robot.
//
//   rafiq "build passed"
//   rafiq focus 25
//   rafiq toast "deploy done" --kind copy
//   rafiq sleep
//
// It reads the address and token the menu bar app already has, so there is
// nothing separate to configure and no second copy of the token anywhere.

func pref(_ key: String) -> String {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/defaults")
    p.arguments = ["read", "in.iotcart.rafiq", key]
    let pipe = Pipe(); p.standardOutput = pipe; p.standardError = Pipe()
    try? p.run(); p.waitUntilExit()
    return String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8)?
        .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
}

func token() -> String {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/security")
    p.arguments = ["find-generic-password", "-s", "in.iotcart.rafiq", "-a", "token", "-w"]
    let pipe = Pipe(); p.standardOutput = pipe; p.standardError = Pipe()
    try? p.run(); p.waitUntilExit()
    return String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8)?
        .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
}

func post(_ path: String, _ fields: [String: String]) -> Int {
    let ip = pref("deviceIP")
    guard !ip.isEmpty, let url = URL(string: "http://\(ip)\(path)") else {
        FileHandle.standardError.write("rafiq: no address set. Open Rafiq and set one.\n".data(using: .utf8)!)
        return 2
    }
    var req = URLRequest(url: url)
    req.httpMethod = "POST"
    req.timeoutInterval = 6
    req.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
    let t = token()
    if !t.isEmpty { req.setValue(t, forHTTPHeaderField: "X-Rafiq-Token") }
    var allowed = CharacterSet.alphanumerics
    allowed.insert(charactersIn: "-._~")
    req.httpBody = fields
        .map { "\($0)=\($1.addingPercentEncoding(withAllowedCharacters: allowed) ?? "")" }
        .joined(separator: "&").data(using: .utf8)

    var code = 0
    let sem = DispatchSemaphore(value: 0)
    URLSession.shared.dataTask(with: req) { _, r, _ in
        code = (r as? HTTPURLResponse)?.statusCode ?? 0
        sem.signal()
    }.resume()
    _ = sem.wait(timeout: .now() + 8)

    switch code {
    case 200: return 0
    case 401, 403:
        FileHandle.standardError.write("rafiq: not paired with this robot.\n".data(using: .utf8)!)
        return 3
    default:
        FileHandle.standardError.write("rafiq: could not reach it.\n".data(using: .utf8)!)
        return 1
    }
}

// Cut to the same 84 bytes the robot keeps, on a character boundary.
func clip(_ s: String) -> String {
    if s.utf8.count <= 84 { return s }
    var out = ""; var n = 0
    for ch in s {
        let c = String(ch).utf8.count
        if n + c > 81 { break }
        out.append(ch); n += c
    }
    return out + "\u{2026}"
}

let args = Array(CommandLine.arguments.dropFirst())
guard let first = args.first else {
    print("""
    rafiq  -  send something to the robot

      rafiq "build passed"         put it on the screen
      rafiq toast "saved" [kind]   show it briefly (copy, paste, break)
      rafiq focus <minutes>        start a focus run, 0 to stop
      rafiq relax on|off           the screensaver
      rafiq sleep                  deep sleep, power to wake it
      rafiq update                 look for new firmware
    """)
    exit(0)
}

var rc = 0
switch first {
case "toast":
    let text = args.count > 1 ? args[1] : ""
    let kind = args.count > 2 ? args[2] : "note"
    rc = post("/api/toast", ["m": clip(text), "k": kind, "s": "5"])
case "focus":
    rc = post("/api/focus", ["m": args.count > 1 ? args[1] : "25"])
case "relax":
    rc = post("/api/relax", ["a": (args.count > 1 && args[1] == "off") ? "0" : "1"])
case "sleep":
    rc = post("/api/deepsleep", [:])
case "update":
    rc = post("/api/update", [:])
default:
    // anything else is simply the thing to say
    rc = post("/api/msg", ["m": clip(args.joined(separator: " "))])
}
exit(Int32(rc))
