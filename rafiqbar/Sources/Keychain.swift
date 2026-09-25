import Foundation

/// The pairing token is what stops anyone else on the network driving the
/// robot, so it lives in the keychain rather than in preferences where any
/// process could read it off disk.
enum Keychain {
    private static let service = "in.iotcart.rafiq"

    static func set(_ value: String, for account: String) {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                kSecAttrService as String: service,
                                kSecAttrAccount as String: account]
        SecItemDelete(q as CFDictionary)
        guard !value.isEmpty, let data = value.data(using: .utf8) else { return }
        var add = q
        add[kSecValueData as String] = data
        // Needed after the first unlock so a token still works when the app
        // launches at login, but never synced to another machine.
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        SecItemAdd(add as CFDictionary, nil)
    }

    static func get(_ account: String) -> String {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                kSecAttrService as String: service,
                                kSecAttrAccount as String: account,
                                kSecReturnData as String: true,
                                kSecMatchLimit as String: kSecMatchLimitOne]
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess,
              let d = out as? Data, let s = String(data: d, encoding: .utf8) else { return "" }
        return s
    }
}
