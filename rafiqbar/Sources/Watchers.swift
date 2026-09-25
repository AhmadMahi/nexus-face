import Foundation
import AppKit
import CoreAudio
import CoreMediaIO

/// Locking the screen.
///
/// `CGSession -suspend`, the route every guide still names, **no longer
/// exists** on current macOS. What works is `SACLockScreenImmediate` from
/// Apple's login framework: private, but it is what lock utilities have
/// used for years and it needs no permission. If Apple ever withdraws it,
/// the fallback sleeps the display, which locks provided the Mac is set to
/// ask for a password after sleep.
enum Screen {
    @discardableResult
    static func lock() -> Bool {
        for path in ["/System/Library/PrivateFrameworks/login.framework/Versions/Current/login",
                     "/System/Library/PrivateFrameworks/login.framework/login"] {
            guard let h = dlopen(path, RTLD_LAZY) else { continue }
            if let sym = dlsym(h, "SACLockScreenImmediate") {
                typealias Fn = @convention(c) () -> Int32
                _ = unsafeBitCast(sym, to: Fn.self)()
                return true
            }
        }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/pmset")
        p.arguments = ["displaysleepnow"]
        try? p.run()
        return false
    }
}

/// Is the built-in camera or microphone live?
///
/// Both answers come from one system flag per device, which is readable
/// without any camera or microphone permission. Nothing is heard, nothing
/// is seen, and nothing is recorded: the only thing this knows is whether
/// a device is switched on.
///
/// Only the built-in pair is watched, picked out by transport type rather
/// than by name, so virtual devices from Zoom, Teams and the like cannot
/// raise a false alarm by sitting open all day.
@MainActor
final class AVWatch: ObservableObject {
    @Published private(set) var cam = false
    @Published private(set) var mic = false

    private var timer: Timer?
    private var onChange: ((Bool, Bool) -> Void)?

    func start(_ onChange: @escaping (Bool, Bool) -> Void) {
        self.onChange = onChange
        stop()
        tick()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
    }
    func stop() {
        timer?.invalidate(); timer = nil
        if cam || mic { cam = false; mic = false; onChange?(false, false) }
    }

    private func tick() {
        let c = Self.builtInCameraLive()
        let m = Self.builtInMicLive()
        guard c != cam || m != mic else { return }
        cam = c; mic = m
        onChange?(c, m)
    }

    // ---- microphone, through CoreAudio ----
    static func builtInMicLive() -> Bool {
        var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
                                              mScope: kAudioObjectPropertyScopeGlobal,
                                              mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject),
                                             &addr, 0, nil, &size) == noErr, size > 0 else { return false }
        var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject),
                                         &addr, 0, nil, &size, &ids) == noErr else { return false }

        for id in ids {
            guard u32(id, kAudioDevicePropertyTransportType) == kAudioDeviceTransportTypeBuiltIn,
                  hasInput(id) else { continue }
            if u32(id, kAudioDevicePropertyDeviceIsRunningSomewhere) != 0 { return true }
        }
        return false
    }

    private static func u32(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector) -> UInt32 {
        var a = AudioObjectPropertyAddress(mSelector: sel,
                                           mScope: kAudioObjectPropertyScopeGlobal,
                                           mElement: kAudioObjectPropertyElementMain)
        var v: UInt32 = 0
        var s = UInt32(MemoryLayout<UInt32>.size)
        guard AudioObjectGetPropertyData(id, &a, 0, nil, &s, &v) == noErr else { return 0 }
        return v
    }

    /// A device with no input channels is a speaker, not a microphone.
    private static func hasInput(_ id: AudioObjectID) -> Bool {
        var a = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreamConfiguration,
                                           mScope: kAudioDevicePropertyScopeInput,
                                           mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(id, &a, 0, nil, &size) == noErr, size > 0 else { return false }
        let buf = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: 8)
        defer { buf.deallocate() }
        guard AudioObjectGetPropertyData(id, &a, 0, nil, &size, buf) == noErr else { return false }
        var ch = 0
        for b in UnsafeMutableAudioBufferListPointer(buf.assumingMemoryBound(to: AudioBufferList.self)) {
            ch += Int(b.mNumberChannels)
        }
        return ch > 0
    }

    // ---- camera, through CoreMediaIO ----
    static func builtInCameraLive() -> Bool {
        var addr = CMIOObjectPropertyAddress(
            mSelector: CMIOObjectPropertySelector(kCMIOHardwarePropertyDevices),
            mScope: CMIOObjectPropertyScope(kCMIOObjectPropertyScopeGlobal),
            mElement: CMIOObjectPropertyElement(kCMIOObjectPropertyElementMain))
        var size: UInt32 = 0
        guard CMIOObjectGetPropertyDataSize(CMIOObjectID(kCMIOObjectSystemObject),
                                            &addr, 0, nil, &size) == noErr, size > 0 else { return false }
        var ids = [CMIOObjectID](repeating: 0, count: Int(size) / MemoryLayout<CMIOObjectID>.size)
        var used: UInt32 = 0
        guard CMIOObjectGetPropertyData(CMIOObjectID(kCMIOObjectSystemObject),
                                        &addr, 0, nil, size, &used, &ids) == noErr else { return false }

        for id in ids {
            guard cu32(id, kCMIODevicePropertyTransportType) == kAudioDeviceTransportTypeBuiltIn
            else { continue }
            if cu32(id, kCMIODevicePropertyDeviceIsRunningSomewhere) != 0 { return true }
        }
        return false
    }

    private static func cu32(_ id: CMIOObjectID, _ sel: Int) -> UInt32 {
        var a = CMIOObjectPropertyAddress(
            mSelector: CMIOObjectPropertySelector(sel),
            mScope: CMIOObjectPropertyScope(kCMIOObjectPropertyScopeGlobal),
            mElement: CMIOObjectPropertyElement(kCMIOObjectPropertyElementMain))
        var v: UInt32 = 0
        var used: UInt32 = 0
        guard CMIOObjectGetPropertyData(id, &a, 0, nil, UInt32(MemoryLayout<UInt32>.size),
                                        &used, &v) == noErr else { return 0 }
        return v
    }
}
