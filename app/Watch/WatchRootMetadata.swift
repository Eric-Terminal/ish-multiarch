import Foundation

struct WatchRootCopyRequest: Codable, Equatable, Sendable {
    let token: UUID
    let sourceName: String

    var tokenString: String {
        token.uuidString.lowercased()
    }
}

struct WatchRootMetadata: Codable, Equatable, Sendable {
    enum RenameError: LocalizedError, Equatable {
        case missingRoot
        case emptyName
        case nameTooLong
        case invalidCharacters
        case duplicateName

        var errorDescription: String? {
            switch self {
            case .missingRoot:
                return "这个 Linux 文件系统已不存在"
            case .emptyName:
                return "名称不能为空"
            case .nameTooLong:
                return "名称不能超过 40 个字符"
            case .invalidCharacters:
                return "名称不能包含换行或控制字符"
            case .duplicateName:
                return "已有同名的 Linux 文件系统"
            }
        }
    }

    static let maximumDisplayNameLength = 40

    private(set) var aliases: [String: String]
    private(set) var selectedRootName: String?
    private(set) var pendingCopy: WatchRootCopyRequest?

    init(
        aliases: [String: String] = [:],
        selectedRootName: String? = nil,
        pendingCopy: WatchRootCopyRequest? = nil
    ) {
        self.aliases = aliases
        self.selectedRootName = selectedRootName
        self.pendingCopy = pendingCopy
    }

    var pendingCopySource: String? {
        pendingCopy?.sourceName
    }

    func displayName(for rootName: String) -> String {
        aliases[rootName] ?? rootName
    }

    mutating func rename(
        _ rootName: String,
        to rawName: String,
        existingRootNames: [String]
    ) throws {
        guard existingRootNames.contains(rootName) else {
            throw RenameError.missingRoot
        }
        let name = rawName.trimmingCharacters(
            in: .whitespacesAndNewlines)
        guard !name.isEmpty else {
            throw RenameError.emptyName
        }
        guard name.count <= Self.maximumDisplayNameLength else {
            throw RenameError.nameTooLong
        }
        guard name.rangeOfCharacter(
            from: .controlCharacters) == nil else {
            throw RenameError.invalidCharacters
        }
        let hasDuplicate = existingRootNames.contains { otherRoot in
            guard otherRoot != rootName else { return false }
            return otherRoot.caseInsensitiveCompare(name) == .orderedSame ||
                displayName(for: otherRoot)
                    .caseInsensitiveCompare(name) == .orderedSame
        }
        guard !hasDuplicate else {
            throw RenameError.duplicateName
        }
        if name == rootName {
            aliases.removeValue(forKey: rootName)
        } else {
            aliases[rootName] = name
        }
        reconcileAliases(existingRootNames: existingRootNames)
    }

    mutating func restoreDefaultName(
        for rootName: String,
        existingRootNames: [String]
    ) {
        aliases.removeValue(forKey: rootName)
        reconcileAliases(existingRootNames: existingRootNames)
    }

    mutating func select(_ rootName: String) {
        selectedRootName = rootName
        pendingCopy = nil
    }

    mutating func scheduleCopy(
        of rootName: String,
        token: UUID = UUID()
    ) {
        pendingCopy = WatchRootCopyRequest(
            token: token,
            sourceName: rootName)
    }

    mutating func cancelScheduledCopy() {
        pendingCopy = nil
    }

    mutating func recordImmediateCopyAndSelect(
        from sourceName: String,
        to destinationName: String,
        existingRootNames: [String]
    ) {
        recordCopiedAlias(
            from: sourceName,
            to: destinationName,
            existingRootNames: existingRootNames)
        select(destinationName)
    }

    @discardableResult
    mutating func completeScheduledCopy(
        _ request: WatchRootCopyRequest,
        destinationName: String,
        existingRootNames: [String]
    ) -> Bool {
        guard pendingCopy == request else {
            return false
        }
        recordCopiedAlias(
            from: request.sourceName,
            to: destinationName,
            existingRootNames: existingRootNames)
        selectedRootName = destinationName
        pendingCopy = nil
        return true
    }

    mutating func remove(_ rootName: String) {
        aliases.removeValue(forKey: rootName)
        if pendingCopy?.sourceName == rootName {
            pendingCopy = nil
        }
    }

    mutating func reconcileAliases(existingRootNames: [String]) {
        let existing = Set(existingRootNames)
        aliases = aliases.filter { existing.contains($0.key) }

        var usedNames = Set(existingRootNames.map { $0.lowercased() })
        for rootName in existingRootNames {
            guard let storedAlias = aliases[rootName] else { continue }
            let normalized = storedAlias.trimmingCharacters(
                in: .whitespacesAndNewlines)
            if normalized.isEmpty ||
                    normalized.caseInsensitiveCompare(rootName) ==
                        .orderedSame {
                aliases.removeValue(forKey: rootName)
                continue
            }
            let unique = uniqueDisplayName(
                normalized,
                usedNames: usedNames)
            aliases[rootName] = unique
            usedNames.insert(unique.lowercased())
        }
    }

    mutating func removeMissingRoots(_ existingRootNames: [String]) {
        reconcileAliases(existingRootNames: existingRootNames)
        let existing = Set(existingRootNames)
        if let sourceName = pendingCopy?.sourceName,
           !existing.contains(sourceName) {
            pendingCopy = nil
        }
    }

    private mutating func recordCopiedAlias(
        from sourceName: String,
        to destinationName: String,
        existingRootNames: [String]
    ) {
        if aliases[sourceName] != nil {
            aliases[destinationName] =
                "\(displayName(for: sourceName)) 副本"
        }
        reconcileAliases(
            existingRootNames:
                Array(Set(existingRootNames + [destinationName])).sorted())
    }

    private func uniqueDisplayName(
        _ requestedName: String,
        usedNames: Set<String>
    ) -> String {
        let baseName = String(
            requestedName.prefix(Self.maximumDisplayNameLength))
        if !usedNames.contains(baseName.lowercased()) {
            return baseName
        }

        var suffix = 2
        while true {
            let suffixText = " \(suffix)"
            let prefixLength = max(
                0,
                Self.maximumDisplayNameLength - suffixText.count)
            let candidate =
                String(baseName.prefix(prefixLength)) + suffixText
            if !usedNames.contains(candidate.lowercased()) {
                return candidate
            }
            suffix += 1
        }
    }
}

enum WatchRootMetadataPersistence {
    static let storageKey = "watchRootMetadata"

    static func load(
        from defaults: UserDefaults,
        legacySelectedRootName: String?
    ) -> WatchRootMetadata {
        guard let data = defaults.data(forKey: storageKey),
              let metadata = try? JSONDecoder().decode(
                WatchRootMetadata.self,
                from: data) else {
            return WatchRootMetadata(
                selectedRootName: legacySelectedRootName)
        }
        return metadata
    }

    static func save(
        _ metadata: WatchRootMetadata,
        to defaults: UserDefaults
    ) {
        // 字典、字符串和 UUID 都由 Foundation 直接编码，不存在业务失败分支。
        let data = try! JSONEncoder().encode(metadata)
        defaults.set(data, forKey: storageKey)
    }
}
