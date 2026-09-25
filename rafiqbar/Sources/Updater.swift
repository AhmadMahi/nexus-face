import Foundation
import AppKit

/// Updating Rafiq itself from the same GitHub repo the firmware comes from.
///
/// App releases are tagged `app-vX.Y.Z` so they sit beside the firmware's
/// own `vX.Y.Z` tags without either triggering the other's build.
///
/// Worth being plain about what is and is not checked here. The download
/// comes over HTTPS from a host that has to belong to GitHub, and the repo
/// is fixed in this file rather than read from anywhere. But Rafiq is ad
/// hoc signed, not signed with an Apple developer certificate, so there is
/// no signature to verify beyond that. The trust is the transport and the
/// pinned repository.
@MainActor
final class Updater: ObservableObject {
    static let shared = Updater()
    static let repo = "AhmadMahi/nexus-face"
    private static let tagPrefix = "app-v"

    enum Phase: Equatable {
        case idle, checking, none, found(String), downloading, installing, failed(String)
    }
    @Published var phase: Phase = .idle

    var current: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0"
    }

    func check(andInstall: Bool) async {
        phase = .checking
        do {
            guard let rel = try await latest() else { phase = .none; return }
            guard newer(rel.version, than: current) else { phase = .none; return }
            guard andInstall else { phase = .found(rel.version); return }
            try await install(rel)
        } catch {
            phase = .failed("Could not check")
        }
    }

    // ---------------------------------------------------------------

    private struct Rel { let version: String; let dmg: URL }

    private func latest() async throws -> Rel? {
        var req = URLRequest(url: URL(string: "https://api.github.com/repos/\(Self.repo)/releases?per_page=30")!)
        req.timeoutInterval = 12
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        let (data, _) = try await URLSession.shared.data(for: req)
        guard let arr = try JSONSerialization.jsonObject(with: data) as? [[String: Any]] else { return nil }

        var best: Rel?
        for r in arr {
            guard let tag = r["tag"] as? String ?? r["tag_name"] as? String,
                  tag.hasPrefix(Self.tagPrefix),
                  (r["draft"] as? Bool) != true else { continue }
            let v = String(tag.dropFirst(Self.tagPrefix.count))
            guard let assets = r["assets"] as? [[String: Any]] else { continue }
            for a in assets {
                guard let name = a["name"] as? String, name.hasSuffix(".dmg"),
                      let s = a["browser_download_url"] as? String,
                      let u = URL(string: s), Self.isGitHub(u) else { continue }
                if best == nil || newer(v, than: best!.version) { best = Rel(version: v, dmg: u) }
                break
            }
        }
        return best
    }

    /// The download has to come from GitHub itself, not from wherever a
    /// release body happens to point.
    private static func isGitHub(_ u: URL) -> Bool {
        guard u.scheme == "https", let h = u.host?.lowercased() else { return false }
        return h == "github.com" || h.hasSuffix(".github.com")
            || h == "objects.githubusercontent.com" || h.hasSuffix(".githubusercontent.com")
    }

    func newer(_ a: String, than b: String) -> Bool {
        let x = a.split(separator: ".").map { Int($0) ?? 0 }
        let y = b.split(separator: ".").map { Int($0) ?? 0 }
        for i in 0..<max(x.count, y.count) {
            let l = i < x.count ? x[i] : 0, r = i < y.count ? y[i] : 0
            if l != r { return l > r }
        }
        return false
    }

    // ---------------------------------------------------------------

    private func install(_ rel: Rel) async throws {
        phase = .downloading
        let (tmp, resp) = try await URLSession.shared.download(from: rel.dmg)
        guard (resp as? HTTPURLResponse)?.statusCode == 200 else {
            phase = .failed("Download failed"); return
        }
        let work = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("rafiq-update-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: work, withIntermediateDirectories: true)
        let dmg = work.appendingPathComponent("Rafiq.dmg")
        try FileManager.default.moveItem(at: tmp, to: dmg)

        phase = .installing
        let mnt = work.appendingPathComponent("mnt")
        guard run("/usr/bin/hdiutil",
                  ["attach", "-quiet", "-nobrowse", "-readonly", "-mountpoint", mnt.path, dmg.path]) else {
            phase = .failed("Could not open the image"); return
        }
        let newApp = mnt.appendingPathComponent("Rafiq.app")
        let staged = work.appendingPathComponent("Rafiq.app")
        let copied = run("/usr/bin/ditto", [newApp.path, staged.path])
        _ = run("/usr/bin/hdiutil", ["detach", "-quiet", mnt.path])
        guard copied, FileManager.default.fileExists(atPath: staged.path) else {
            phase = .failed("The image had no app in it"); return
        }

        // A running bundle cannot replace itself, so a small script waits
        // for this process to go and then does the swap and relaunch.
        let here = Bundle.main.bundleURL
        let script = work.appendingPathComponent("swap.sh")
        let sh = """
        #!/bin/bash
        for _ in $(seq 1 100); do
          kill -0 \(ProcessInfo.processInfo.processIdentifier) 2>/dev/null || break
          sleep 0.2
        done
        /bin/rm -rf "\(here.path)"
        /usr/bin/ditto "\(staged.path)" "\(here.path)" || exit 1
        /usr/bin/open "\(here.path)"
        /bin/rm -rf "\(work.path)"
        """
        try sh.write(to: script, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: script.path)

        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/bash")
        p.arguments = [script.path]
        try p.run()
        NSApp.terminate(nil)
    }

    @discardableResult
    private func run(_ tool: String, _ args: [String]) -> Bool {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        p.standardOutput = Pipe(); p.standardError = Pipe()
        do { try p.run() } catch { return false }
        p.waitUntilExit()
        return p.terminationStatus == 0
    }
}
