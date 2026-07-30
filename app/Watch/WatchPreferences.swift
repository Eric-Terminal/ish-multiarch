import Foundation

enum WatchPreferenceKeys {
    static let colorScheme = "watchColorScheme"
    static let terminalFontSize = "watchTerminalFontSize"
    static let customHostnameEnabled = "watchCustomHostnameEnabled"
    static let customHostname = "watchCustomHostname"
    static let bootCommand = "watchBootCommand"
    static let launchCommand = "watchLaunchCommand"
}

enum WatchPreferences {
    enum ValidationIssue: Equatable {
        case empty
        case tooLong
        case controlCharacter
    }

    static let defaultBootCommand = "exec /sbin/init"
    static let defaultLaunchCommand = "exec /bin/login -f root"
    static let terminalFontSizeRange = 7...24
    static let maximumHostnameBytes = 64
    static let maximumLaunchCommandBytes = 4095

    static func hostname(
        deviceName: String,
        usesCustomHostname: Bool,
        customHostname: String
    ) -> String {
        let automatic = normalizedHostname(deviceName) ?? "iSH"
        guard usesCustomHostname,
              let custom = normalizedHostname(customHostname) else {
            return automatic
        }
        return custom
    }

    static func launchCommand(_ value: String?) -> String {
        guard let value else { return defaultLaunchCommand }
        let command = value.trimmingCharacters(
            in: .whitespacesAndNewlines)
        guard launchCommandValidationIssue(command) == nil else {
            return defaultLaunchCommand
        }
        return command
    }

    static func bootCommand(_ value: String?) -> String {
        guard let value else { return defaultBootCommand }
        let command = value.trimmingCharacters(
            in: .whitespacesAndNewlines)
        guard launchCommandValidationIssue(command) == nil else {
            return defaultBootCommand
        }
        return command
    }

    static func normalizedHostname(_ value: String) -> String? {
        let hostname = value.trimmingCharacters(
            in: .whitespacesAndNewlines)
        guard hostnameValidationIssue(hostname) == nil else {
            return nil
        }
        return hostname
    }

    static func hostnameValidationIssue(
        _ value: String
    ) -> ValidationIssue? {
        if value.isEmpty {
            return .empty
        }
        if value.utf8.count > maximumHostnameBytes {
            return .tooLong
        }
        if value.unicodeScalars.contains(where: {
            CharacterSet.controlCharacters.contains($0)
        }) {
            return .controlCharacter
        }
        return nil
    }

    static func launchCommandValidationIssue(
        _ value: String
    ) -> ValidationIssue? {
        if value.isEmpty {
            return .empty
        }
        if value.utf8.count > maximumLaunchCommandBytes {
            return .tooLong
        }
        if value.unicodeScalars.contains(where: { $0.value == 0 }) {
            return .controlCharacter
        }
        return nil
    }
}
