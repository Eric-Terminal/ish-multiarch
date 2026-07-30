import CoreFoundation
import Foundation

#if canImport(Darwin)
import Darwin
#endif

enum WatchProcPreference: CaseIterable {
    case fontFamily
    case fontSize
    case theme
    case colorScheme
    case cursorStyle
    case blinkCursor
    case launchCommand
    case bootCommand
    case hostnameOverride

    var friendlyName: String {
        switch self {
        case .fontFamily:
            return "font_family"
        case .fontSize:
            return "font_size"
        case .theme:
            return "theme"
        case .colorScheme:
            return "color_scheme"
        case .cursorStyle:
            return "cursor_style"
        case .blinkCursor:
            return "blink_cursor"
        case .launchCommand:
            return "launch_command"
        case .bootCommand:
            return "boot_command"
        case .hostnameOverride:
            return "hostname_override"
        }
    }

    var underlyingName: String {
        switch self {
        case .fontFamily:
            return WatchTerminalAppearancePreferenceKey.font
        case .fontSize:
            return WatchPreferenceKeys.terminalFontSize
        case .theme:
            return WatchTerminalAppearancePreferenceKey.palette
        case .colorScheme:
            return WatchPreferenceKeys.colorScheme
        case .cursorStyle:
            return WatchTerminalAppearancePreferenceKey.cursorShape
        case .blinkCursor:
            return WatchTerminalAppearancePreferenceKey.cursorBlink
        case .launchCommand:
            return WatchPreferenceKeys.launchCommand
        case .bootCommand:
            return WatchPreferenceKeys.bootCommand
        case .hostnameOverride:
            // 主机名由开关和值共同决定，因此使用不对应 UserDefaults 的虚拟键。
            return "watchHostnameOverride"
        }
    }

    static func preference(
        underlyingName: String
    ) -> WatchProcPreference? {
        allCases.first { $0.underlyingName == underlyingName }
    }

    static func preference(
        friendlyName: String
    ) -> WatchProcPreference? {
        allCases.first { $0.friendlyName == friendlyName }
    }
}

struct WatchProcPreferenceStore {
    let defaults: UserDefaults
    let automaticHostname: String

    static var underlyingNames: [String] {
        WatchProcPreference.allCases.map(\.underlyingName)
    }

    func friendlyName(for underlyingName: String) -> String? {
        WatchProcPreference.preference(
            underlyingName: underlyingName)?.friendlyName
    }

    func underlyingName(for friendlyName: String) -> String? {
        WatchProcPreference.preference(
            friendlyName: friendlyName)?.underlyingName
    }

    func jsonData(for underlyingName: String) -> Data? {
        guard let preference = WatchProcPreference.preference(
            underlyingName: underlyingName
        ) else {
            return nil
        }
        guard var data = try? JSONSerialization.data(
            withJSONObject: value(for: preference),
            options: [
                .fragmentsAllowed,
                .sortedKeys,
                .withoutEscapingSlashes,
            ]
        ) else {
            return nil
        }
        data.append(0x0a)
        return data
    }

    @discardableResult
    func setJSONData(
        _ data: Data,
        for underlyingName: String
    ) -> Bool {
        guard let preference = WatchProcPreference.preference(
            underlyingName: underlyingName),
              let value = try? JSONSerialization.jsonObject(
                with: data,
                options: .fragmentsAllowed) else {
            return false
        }

        switch preference {
        case .fontFamily:
            guard let string = value as? String,
                  WatchTerminalFontChoice(rawValue: string) != nil else {
                return false
            }
            defaults.set(string, forKey: preference.underlyingName)
        case .fontSize:
            guard let number = strictJSONInteger(value),
                  number.compare(
                    NSNumber(
                        value: WatchPreferences
                            .terminalFontSizeRange.lowerBound)) !=
                    .orderedAscending,
                  number.compare(
                    NSNumber(
                        value: WatchPreferences
                            .terminalFontSizeRange.upperBound)) !=
                    .orderedDescending else {
                return false
            }
            defaults.set(
                Double(number.intValue),
                forKey: preference.underlyingName)
        case .theme:
            guard let string = value as? String,
                  let selectionID = normalizedThemeSelection(string) else {
                return false
            }
            defaults.set(selectionID, forKey: preference.underlyingName)
        case .colorScheme:
            guard let string = value as? String,
                  ["system", "light", "dark"].contains(string) else {
                return false
            }
            defaults.set(string, forKey: preference.underlyingName)
        case .cursorStyle:
            guard let string = value as? String,
                  WatchTerminalCursorShape(rawValue: string) != nil else {
                return false
            }
            defaults.set(string, forKey: preference.underlyingName)
        case .blinkCursor:
            guard let bool = strictBool(value) else { return false }
            defaults.set(bool, forKey: preference.underlyingName)
        case .launchCommand, .bootCommand:
            // Watch 会话由 /bin/sh -c 执行单条命令，不接受 iOS argv 数组。
            guard let string = value as? String else { return false }
            let command = string.trimmingCharacters(
                in: .whitespacesAndNewlines)
            guard WatchPreferences.launchCommandValidationIssue(command) ==
                    nil else {
                return false
            }
            defaults.set(command, forKey: preference.underlyingName)
        case .hostnameOverride:
            guard let string = value as? String,
                  let hostname = WatchPreferences.normalizedHostname(
                    string) else {
                return false
            }
            defaults.set(
                hostname,
                forKey: WatchPreferenceKeys.customHostname)
            defaults.set(
                true,
                forKey: WatchPreferenceKeys.customHostnameEnabled)
        }
        return true
    }

    @discardableResult
    func removeValue(for underlyingName: String) -> Bool {
        guard let preference = WatchProcPreference.preference(
            underlyingName: underlyingName
        ) else {
            return false
        }
        if preference == .hostnameOverride {
            defaults.removeObject(
                forKey: WatchPreferenceKeys.customHostname)
            defaults.removeObject(
                forKey: WatchPreferenceKeys.customHostnameEnabled)
        } else {
            defaults.removeObject(forKey: preference.underlyingName)
        }
        return true
    }

    private func value(for preference: WatchProcPreference) -> Any {
        switch preference {
        case .fontFamily:
            return strictString(forKey: preference.underlyingName)
                .flatMap(WatchTerminalFontChoice.init(rawValue:))?
                .rawValue ??
                WatchTerminalFontChoice.systemMonospaced.rawValue
        case .fontSize:
            let value = strictNumber(
                defaults.object(forKey: preference.underlyingName))
            guard let value,
                  value.rounded() == value,
                  value >= Double(
                    WatchPreferences.terminalFontSizeRange.lowerBound),
                  value <= Double(
                    WatchPreferences.terminalFontSizeRange.upperBound) else {
                return 11.0
            }
            return value
        case .theme:
            let candidate =
                strictString(forKey: preference.underlyingName) ??
                WatchTerminalBuiltInThemeID.defaultTheme.rawValue
            return normalizedThemeSelection(candidate) ??
                WatchTerminalBuiltInThemeID.defaultTheme.rawValue
        case .colorScheme:
            let value = strictString(
                forKey: preference.underlyingName) ?? "system"
            return ["system", "light", "dark"].contains(value) ?
                value : "system"
        case .cursorStyle:
            return strictString(forKey: preference.underlyingName)
                .flatMap(WatchTerminalCursorShape.init(rawValue:))?
                .rawValue ??
                WatchTerminalCursorShape.block.rawValue
        case .blinkCursor:
            return strictBool(
                defaults.object(forKey: preference.underlyingName)) ??
                false
        case .launchCommand:
            return WatchPreferences.launchCommand(
                strictString(forKey: preference.underlyingName))
        case .bootCommand:
            return WatchPreferences.bootCommand(
                strictString(forKey: preference.underlyingName))
        case .hostnameOverride:
            return WatchPreferences.hostname(
                deviceName: automaticHostname,
                usesCustomHostname: strictBool(
                    defaults.object(
                        forKey: WatchPreferenceKeys
                            .customHostnameEnabled)) ?? false,
                customHostname: strictString(
                    forKey: WatchPreferenceKeys.customHostname) ?? "")
        }
    }

    private func normalizedThemeSelection(
        _ selectionID: String
    ) -> String? {
        if let builtIn = WatchTerminalBuiltInThemeID(
            selectionID: selectionID
        ) {
            return builtIn.rawValue
        }
        return WatchTerminalThemeStore.theme(
            for: selectionID,
            in: WatchTerminalThemeStore.load(from: defaults)
        )?.selectionID
    }

    private func strictString(forKey key: String) -> String? {
        defaults.object(forKey: key) as? String
    }

    private func strictNumber(_ value: Any?) -> Double? {
        guard let number = value as? NSNumber,
              CFGetTypeID(number) != CFBooleanGetTypeID() else {
            return nil
        }
        let result = number.doubleValue
        return result.isFinite ? result : nil
    }

    private func strictJSONInteger(_ value: Any?) -> NSNumber? {
        guard let number = value as? NSNumber,
              CFGetTypeID(number) != CFBooleanGetTypeID(),
              [
                "c", "s", "i", "l", "q",
                "C", "S", "I", "L", "Q",
              ].contains(String(cString: number.objCType)) else {
            return nil
        }
        return number
    }

    private func strictBool(_ value: Any?) -> Bool? {
        guard let number = value as? NSNumber,
              CFGetTypeID(number) == CFBooleanGetTypeID() else {
            return nil
        }
        return number.boolValue
    }
}

private final class WatchPlatformBridgeContext: @unchecked Sendable {
    static let shared = WatchPlatformBridgeContext()

    private let lock = NSLock()
    private var automaticHostname = "iSH"

    func update(automaticHostname: String) {
        lock.lock()
        self.automaticHostname = automaticHostname
        lock.unlock()
    }

    func currentAutomaticHostname() -> String {
        lock.lock()
        defer { lock.unlock() }
        return automaticHostname
    }
}

enum WatchPlatformBridge {
#if os(watchOS)
    static func install(automaticHostname: String) {
        WatchPlatformBridgeContext.shared.update(
            automaticHostname: automaticHostname)

        ish_install_user_defaults_callbacks(
            watchGetAllDefaultsKeys,
            watchGetFriendlyName,
            watchGetUnderlyingName,
            watchGetUserDefault,
            watchSetUserDefault,
            watchRemoveUserDefault)
    }
#endif
}

private func watchPreferenceStore() -> WatchProcPreferenceStore {
    WatchProcPreferenceStore(
        defaults: .standard,
        automaticHostname:
            WatchPlatformBridgeContext.shared.currentAutomaticHostname())
}

private func watchGetAllDefaultsKeys()
    -> UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>? {
    let names = WatchProcPreferenceStore.underlyingNames
    let capacity = names.count + 1
    guard let allocation = calloc(
        capacity,
        MemoryLayout<UnsafeMutablePointer<CChar>?>.stride
    ) else {
        return nil
    }
    let entries = allocation.bindMemory(
        to: UnsafeMutablePointer<CChar>?.self,
        capacity: capacity)
    for (index, name) in names.enumerated() {
        guard let copy = strdup(name) else {
            for initializedIndex in 0..<index {
                free(entries[initializedIndex])
            }
            free(entries)
            return nil
        }
        entries[index] = copy
    }
    entries[names.count] = nil
    return entries
}

private func watchGetFriendlyName(
    _ name: UnsafePointer<CChar>?
) -> UnsafeMutablePointer<CChar>? {
    guard let name,
          let underlyingName = String(validatingUTF8: name),
          let friendlyName = watchPreferenceStore().friendlyName(
            for: underlyingName) else {
        return nil
    }
    return strdup(friendlyName)
}

private func watchGetUnderlyingName(
    _ name: UnsafePointer<CChar>?
) -> UnsafeMutablePointer<CChar>? {
    guard let name,
          let friendlyName = String(validatingUTF8: name),
          let underlyingName = watchPreferenceStore().underlyingName(
            for: friendlyName) else {
        return nil
    }
    return strdup(underlyingName)
}

private func watchGetUserDefault(
    _ name: UnsafePointer<CChar>?,
    _ buffer: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?,
    _ size: UnsafeMutablePointer<Int>?
) -> Bool {
    guard let name,
          let buffer,
          let size,
          let underlyingName = String(validatingUTF8: name),
          let data = watchPreferenceStore().jsonData(
            for: underlyingName),
          let allocation = malloc(data.count) else {
        return false
    }
    data.copyBytes(
        to: allocation.assumingMemoryBound(to: UInt8.self),
        count: data.count)
    buffer.pointee = allocation.assumingMemoryBound(to: CChar.self)
    size.pointee = data.count
    return true
}

private func watchSetUserDefault(
    _ name: UnsafePointer<CChar>?,
    _ buffer: UnsafeMutablePointer<CChar>?,
    _ size: Int
) -> Bool {
    guard let name,
          let buffer,
          size >= 0,
          let underlyingName = String(validatingUTF8: name) else {
        return false
    }
    return watchPreferenceStore().setJSONData(
        Data(bytes: buffer, count: size),
        for: underlyingName)
}

private func watchRemoveUserDefault(
    _ name: UnsafePointer<CChar>?
) -> Bool {
    guard let name,
          let underlyingName = String(validatingUTF8: name) else {
        return false
    }
    return watchPreferenceStore().removeValue(
        for: underlyingName)
}
