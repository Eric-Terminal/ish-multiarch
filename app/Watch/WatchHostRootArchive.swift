import Foundation

enum WatchHostRootArchiveOperation: Equatable {
    case importing(String)
    case exporting(String)
}

struct WatchHostRootArchiveProgress: Equatable {
    let fraction: Double
    let message: String

    init(fraction: Double, message: String) {
        self.fraction = min(max(fraction, 0), 1)
        self.message = message
    }
}

enum WatchHostRootArchive {
    static func supportsImport(fileName: String) -> Bool {
        guard !fileName.isEmpty,
              fileName != ".",
              fileName != "..",
              !fileName.contains("/"),
              fileName.utf8.count <= 255 else {
            return false
        }
        let lowercaseName = fileName.lowercased()
        return lowercaseName.hasSuffix(".tar") ||
            lowercaseName.hasSuffix(".tar.gz") ||
            lowercaseName.hasSuffix(".tgz")
    }

    static func exportFileName(
        rootName: String,
        date: Date = Date(),
        token: UUID = UUID()
    ) -> String {
        let safeRootName = safeComponent(rootName)
        let formatter = DateFormatter()
        formatter.calendar = Calendar(identifier: .gregorian)
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(secondsFromGMT: 0)
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        let tokenText = token.uuidString
            .replacingOccurrences(of: "-", with: "")
            .lowercased()
        return "ish-\(safeRootName)-\(formatter.string(from: date))-" +
            "\(tokenText).tar.gz"
    }

    private static func safeComponent(_ value: String) -> String {
        var result = ""
        var lastWasSeparator = false
        for scalar in value.unicodeScalars {
            let isASCIIAlphaNumeric =
                (scalar.value >= 48 && scalar.value <= 57) ||
                (scalar.value >= 65 && scalar.value <= 90) ||
                (scalar.value >= 97 && scalar.value <= 122)
            if isASCIIAlphaNumeric || scalar == "-" || scalar == "_" {
                result.unicodeScalars.append(scalar)
                lastWasSeparator = false
            } else if !result.isEmpty && !lastWasSeparator {
                result.append("-")
                lastWasSeparator = true
            }
            if result.utf8.count >= 32 {
                break
            }
        }
        let trimmed = result.trimmingCharacters(
            in: CharacterSet(charactersIn: "-"))
        return trimmed.isEmpty ? "root" : trimmed
    }
}
