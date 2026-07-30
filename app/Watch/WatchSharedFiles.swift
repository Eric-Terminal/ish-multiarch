import Foundation
import Darwin
import CoreTransferable
import UniformTypeIdentifiers

struct WatchSharedFile: Equatable, Identifiable, Sendable {
    let url: URL
    let name: String
    let byteCount: Int
    let modificationDate: Date?
    let deviceID: dev_t
    let inodeNumber: ino_t
    let generationNumber: UInt32

    var id: URL {
        url
    }
}

enum WatchSharedFilesError: LocalizedError, Equatable {
    case documentsDirectoryUnavailable
    case invalidFile
    case fileChanged
    case fileTooLarge
    case tooManyFiles
    case cannotReadFile
    case cannotWriteFile
    case cannotDeleteFile

    var errorDescription: String? {
        switch self {
        case .documentsDirectoryUnavailable:
            return "无法找到 Watch 文稿目录。"
        case .invalidFile:
            return "只能删除共享目录中的普通文件。"
        case .fileChanged:
            return "共享文件已发生变化，请刷新后重试。"
        case .fileTooLarge:
            return "共享文件超过此操作允许的大小。"
        case .tooManyFiles:
            return "共享目录项目过多，请先在终端中整理。"
        case .cannotReadFile:
            return "无法安全读取共享文件。"
        case .cannotWriteFile:
            return "无法安全写入共享文件。"
        case .cannotDeleteFile:
            return "无法安全删除共享文件。"
        }
    }
}

enum WatchSharedFiles {
    static let directoryName = "Shared"
    static let guestMountPath = "/mnt/shared"
    static let maximumDirectoryEntryCount = 256
    private static let rootArchivePartialPrefix = ".ish-"
    private static let rootArchivePartialSuffix = ".tar.gz.partial"

    // C 运行时会在发布 PID 1 前完成唯一挂载，终端无需处理宿主路径。
    static let terminalCommand = "cd /mnt/shared && exec /bin/sh -l"

    static func ensureSystemDirectory(
        fileManager: FileManager = .default
    ) throws -> URL {
        guard let documentsDirectory = fileManager.urls(
            for: .documentDirectory,
            in: .userDomainMask
        ).first else {
            throw WatchSharedFilesError.documentsDirectoryUnavailable
        }
        return try ensureDirectory(
            in: documentsDirectory,
            fileManager: fileManager)
    }

    static func ensureDirectory(
        in documentsDirectory: URL,
        fileManager: FileManager = .default
    ) throws -> URL {
        let directory = documentsDirectory.appendingPathComponent(
            directoryName,
            isDirectory: true)
        try fileManager.createDirectory(
            at: directory,
            withIntermediateDirectories: true)
        return directory
    }

    static func systemFiles(
        fileManager: FileManager = .default
    ) throws -> [WatchSharedFile] {
        let directory = try ensureSystemDirectory(
            fileManager: fileManager)
        return try files(in: directory, fileManager: fileManager)
    }

    static func loadSystemFiles() async throws -> [WatchSharedFile] {
        try await Task.detached(priority: .userInitiated) {
            try systemFiles()
        }.value
    }

    static func cleanupInterruptedRootArchivePartials(
        in directory: URL
    ) throws -> Int {
        let descriptor = open(
            directory.path,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY)
        guard descriptor >= 0 else {
            throw WatchSharedFilesError.cannotDeleteFile
        }
        guard let stream = fdopendir(descriptor) else {
            close(descriptor)
            throw WatchSharedFilesError.cannotDeleteFile
        }
        defer {
            closedir(stream)
        }

        var removedCount = 0
        var scannedCount = 0
        while scannedCount < maximumDirectoryEntryCount {
            errno = 0
            guard let entry = readdir(stream) else {
                guard errno == 0 else {
                    throw WatchSharedFilesError.cannotDeleteFile
                }
                break
            }
            scannedCount += 1
            let name = withUnsafePointer(to: &entry.pointee.d_name) {
                $0.withMemoryRebound(
                    to: CChar.self,
                    capacity: Int(MAXNAMLEN) + 1
                ) {
                    String(cString: $0)
                }
            }
            guard isRootArchivePartialName(name) else {
                continue
            }

            var metadata = stat()
            guard fstatat(
                descriptor,
                name,
                &metadata,
                AT_SYMLINK_NOFOLLOW) == 0 else {
                if errno == ENOENT {
                    continue
                }
                throw WatchSharedFilesError.cannotDeleteFile
            }
            guard metadata.st_mode & S_IFMT == S_IFREG else {
                continue
            }
            guard unlinkat(descriptor, name, 0) == 0 else {
                if errno == ENOENT {
                    continue
                }
                throw WatchSharedFilesError.cannotDeleteFile
            }
            removedCount += 1
        }

        if removedCount > 0 && fsync(descriptor) != 0 &&
                errno != EINVAL && errno != ENOTSUP {
            throw WatchSharedFilesError.cannotDeleteFile
        }
        return removedCount
    }

    static func files(
        in directory: URL,
        fileManager: FileManager = .default
    ) throws -> [WatchSharedFile] {
        let directoryDescriptor = open(
            directory.path,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY)
        guard directoryDescriptor >= 0 else {
            throw WatchSharedFilesError.cannotReadFile
        }
        defer {
            close(directoryDescriptor)
        }

        var enumerationError: Error?
        guard let enumerator = fileManager.enumerator(
            at: directory,
            includingPropertiesForKeys: nil,
            options: [.skipsSubdirectoryDescendants],
            errorHandler: { _, error in
                enumerationError = error
                return false
            }
        ) else {
            throw WatchSharedFilesError.cannotReadFile
        }
        var urls: [URL] = []
        for case let url as URL in enumerator {
            guard urls.count < maximumDirectoryEntryCount else {
                throw WatchSharedFilesError.tooManyFiles
            }
            urls.append(url)
        }
        if enumerationError != nil {
            throw WatchSharedFilesError.cannotReadFile
        }

        return urls.compactMap { url in
            let name = url.lastPathComponent
            var metadata = stat()
            guard !name.hasPrefix("."),
                  isSafeFileName(name),
                  fstatat(
                    directoryDescriptor,
                    name,
                    &metadata,
                    AT_SYMLINK_NOFOLLOW) == 0,
                  metadata.st_mode & S_IFMT == S_IFREG else {
                return nil
            }
            return WatchSharedFile(
                url: url,
                name: name,
                byteCount: metadata.st_size > 0 ?
                    Int(clamping: metadata.st_size) : 0,
                modificationDate: modificationDate(of: metadata),
                deviceID: metadata.st_dev,
                inodeNumber: metadata.st_ino,
                generationNumber: metadata.st_gen)
        }.sorted {
            $0.name.localizedStandardCompare($1.name) == .orderedAscending
        }
    }

    static func deleteSystemFile(
        _ file: WatchSharedFile,
        fileManager: FileManager = .default
    ) throws {
        let directory = try ensureSystemDirectory(
            fileManager: fileManager)
        try delete(
            file,
            from: directory,
            fileManager: fileManager)
    }

    static func readSystemFile(
        _ file: WatchSharedFile,
        maximumByteCount: Int,
        fileManager: FileManager = .default
    ) throws -> Data {
        let directory = try ensureSystemDirectory(
            fileManager: fileManager)
        return try read(
            file,
            from: directory,
            maximumByteCount: maximumByteCount)
    }

    static func writeSystemFile(
        _ data: Data,
        preferredFileName: String,
        fileManager: FileManager = .default
    ) throws -> WatchSharedFile {
        let directory = try ensureSystemDirectory(
            fileManager: fileManager)
        return try write(
            data,
            preferredFileName: preferredFileName,
            to: directory)
    }

    static func read(
        _ file: WatchSharedFile,
        from directory: URL,
        maximumByteCount: Int
    ) throws -> Data {
        guard maximumByteCount >= 0,
              isSafeFileName(file.name),
              file.name == file.url.lastPathComponent,
              file.url.deletingLastPathComponent().standardizedFileURL ==
                directory.standardizedFileURL else {
            throw WatchSharedFilesError.invalidFile
        }

        let directoryDescriptor = open(
            directory.path,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY)
        guard directoryDescriptor >= 0 else {
            throw WatchSharedFilesError.cannotReadFile
        }
        defer {
            close(directoryDescriptor)
        }

        let descriptor = openat(
            directoryDescriptor,
            file.name,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW)
        guard descriptor >= 0 else {
            throw WatchSharedFilesError.cannotReadFile
        }
        let handle = FileHandle(
            fileDescriptor: descriptor,
            closeOnDealloc: true)
        var metadata = stat()
        guard fstat(descriptor, &metadata) == 0,
              metadata.st_mode & S_IFMT == S_IFREG,
              identity(of: metadata, matches: file) else {
            try? handle.close()
            throw WatchSharedFilesError.invalidFile
        }
        guard metadata.st_size >= 0,
              metadata.st_size <= maximumByteCount else {
            try? handle.close()
            throw WatchSharedFilesError.fileTooLarge
        }
        do {
            let readLimit = maximumByteCount == Int.max ?
                maximumByteCount : maximumByteCount + 1
            let data = try handle.read(
                upToCount: readLimit) ?? Data()
            try handle.close()
            guard data.count <= maximumByteCount else {
                throw WatchSharedFilesError.fileTooLarge
            }
            return data
        } catch let error as WatchSharedFilesError {
            throw error
        } catch {
            throw WatchSharedFilesError.cannotReadFile
        }
    }

    static func validate(
        _ file: WatchSharedFile,
        in directory: URL
    ) throws {
        guard isSafeFileName(file.name),
              file.name == file.url.lastPathComponent,
              file.url.deletingLastPathComponent().standardizedFileURL ==
                directory.standardizedFileURL else {
            throw WatchSharedFilesError.invalidFile
        }

        let directoryDescriptor = open(
            directory.path,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY)
        guard directoryDescriptor >= 0 else {
            throw WatchSharedFilesError.cannotReadFile
        }
        defer {
            close(directoryDescriptor)
        }

        let descriptor = openat(
            directoryDescriptor,
            file.name,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW)
        guard descriptor >= 0 else {
            throw WatchSharedFilesError.cannotReadFile
        }
        defer {
            close(descriptor)
        }

        var metadata = stat()
        guard fstat(descriptor, &metadata) == 0,
              metadata.st_mode & S_IFMT == S_IFREG,
              identity(of: metadata, matches: file) else {
            throw WatchSharedFilesError.invalidFile
        }
    }

    static func write(
        _ data: Data,
        preferredFileName: String,
        to directory: URL
    ) throws -> WatchSharedFile {
        let directoryDescriptor = open(
            directory.path,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY)
        guard directoryDescriptor >= 0 else {
            throw WatchSharedFilesError.cannotWriteFile
        }
        defer {
            close(directoryDescriptor)
        }

        let partialName =
            ".ish-theme-\(UUID().uuidString.lowercased()).partial"
        let descriptor = openat(
            directoryDescriptor,
            partialName,
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            S_IRUSR | S_IWUSR)
        guard descriptor >= 0 else {
            throw WatchSharedFilesError.cannotWriteFile
        }

        var descriptorIsOpen = true
        var publishedName: String?
        defer {
            if descriptorIsOpen {
                close(descriptor)
            }
            if let publishedName {
                unlinkat(directoryDescriptor, publishedName, 0)
            }
            unlinkat(directoryDescriptor, partialName, 0)
            if publishedName != nil {
                _ = fsync(directoryDescriptor)
            }
        }

        do {
            try data.withUnsafeBytes { bytes in
                guard var cursor = bytes.baseAddress else {
                    return
                }
                var remaining = bytes.count
                while remaining > 0 {
                    let count = Darwin.write(
                        descriptor,
                        cursor,
                        remaining)
                    guard count > 0 else {
                        throw WatchSharedFilesError.cannotWriteFile
                    }
                    cursor = cursor.advanced(by: count)
                    remaining -= count
                }
            }
            var metadata = stat()
            guard fsync(descriptor) == 0,
                  fstat(descriptor, &metadata) == 0,
                  metadata.st_mode & S_IFMT == S_IFREG else {
                throw WatchSharedFilesError.cannotWriteFile
            }
            guard close(descriptor) == 0 else {
                descriptorIsOpen = false
                throw WatchSharedFilesError.cannotWriteFile
            }
            descriptorIsOpen = false

            let stem = safeJSONFileStem(preferredFileName)
            var suffix = 1
            while suffix < 10_000 {
                let fileName = suffix == 1 ?
                    "\(stem).json" : "\(stem) \(suffix).json"
                if linkat(
                    directoryDescriptor,
                    partialName,
                    directoryDescriptor,
                    fileName,
                    0
                ) == 0 {
                    publishedName = fileName
                    guard fsync(directoryDescriptor) == 0,
                          unlinkat(
                            directoryDescriptor,
                            partialName,
                            0) == 0,
                          fsync(directoryDescriptor) == 0 else {
                        throw WatchSharedFilesError.cannotWriteFile
                    }
                    publishedName = nil
                    return WatchSharedFile(
                        url: directory.appendingPathComponent(fileName),
                        name: fileName,
                        byteCount: data.count,
                        modificationDate: modificationDate(of: metadata),
                        deviceID: metadata.st_dev,
                        inodeNumber: metadata.st_ino,
                        generationNumber: metadata.st_gen)
                }
                guard errno == EEXIST else {
                    throw WatchSharedFilesError.cannotWriteFile
                }
                suffix += 1
            }
            throw WatchSharedFilesError.cannotWriteFile
        } catch let error as WatchSharedFilesError {
            throw error
        } catch {
            throw WatchSharedFilesError.cannotWriteFile
        }
    }

    static func delete(
        _ file: WatchSharedFile,
        from directory: URL,
        fileManager: FileManager = .default
    ) throws {
        guard isSafeFileName(file.name),
              file.name == file.url.lastPathComponent,
              file.url.deletingLastPathComponent().standardizedFileURL ==
                directory.standardizedFileURL else {
            throw WatchSharedFilesError.invalidFile
        }

        let directoryDescriptor = open(
            directory.path,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY)
        guard directoryDescriptor >= 0 else {
            throw WatchSharedFilesError.cannotDeleteFile
        }
        defer {
            close(directoryDescriptor)
        }

        let token = UUID().uuidString.lowercased()
        let pendingName = ".ish-delete-\(token).pending"
        guard renameatx_np(
            directoryDescriptor,
            file.name,
            directoryDescriptor,
            pendingName,
            UInt32(RENAME_EXCL)) == 0 else {
            if errno == ENOENT {
                throw WatchSharedFilesError.fileChanged
            }
            throw WatchSharedFilesError.cannotDeleteFile
        }

        func restoreCapturedFile() -> Bool {
            if renameatx_np(
                directoryDescriptor,
                pendingName,
                directoryDescriptor,
                file.name,
                UInt32(RENAME_EXCL)
            ) == 0 {
                return true
            }

            let recoveryName = "ish-delete-recovery-\(token)"
            _ = renameatx_np(
                directoryDescriptor,
                pendingName,
                directoryDescriptor,
                recoveryName,
                UInt32(RENAME_EXCL))
            return false
        }

        var metadata = stat()
        guard fstatat(
            directoryDescriptor,
            pendingName,
            &metadata,
            AT_SYMLINK_NOFOLLOW) == 0 else {
            _ = restoreCapturedFile()
            throw WatchSharedFilesError.cannotDeleteFile
        }
        guard metadata.st_mode & S_IFMT == S_IFREG,
              identity(of: metadata, matches: file) else {
            guard restoreCapturedFile() else {
                throw WatchSharedFilesError.cannotDeleteFile
            }
            throw WatchSharedFilesError.fileChanged
        }
        guard unlinkat(directoryDescriptor, pendingName, 0) == 0 else {
            _ = restoreCapturedFile()
            throw WatchSharedFilesError.cannotDeleteFile
        }
    }

    private static func isSafeFileName(_ name: String) -> Bool {
        !name.isEmpty &&
            name != "." &&
            name != ".." &&
            !name.contains("/") &&
            !name.unicodeScalars.contains("\0")
    }

    private static func isRootArchivePartialName(
        _ name: String
    ) -> Bool {
        guard name.hasPrefix(rootArchivePartialPrefix),
              name.hasSuffix(rootArchivePartialSuffix) else {
            return false
        }
        let start = name.index(
            name.startIndex,
            offsetBy: rootArchivePartialPrefix.count)
        let end = name.index(
            name.endIndex,
            offsetBy: -rootArchivePartialSuffix.count)
        let body = Array(name[start..<end].utf8)
        let timestampLength = 16
        let tokenLength = 32
        let fixedLength = 1 + timestampLength + 1 + tokenLength
        guard body.count > fixedLength,
              body.count <= 32 + fixedLength else {
            return false
        }

        let tokenStart = body.count - tokenLength
        let timestampStart = tokenStart - 1 - timestampLength
        let rootEnd = timestampStart - 1
        guard body[tokenStart - 1] == Character("-").asciiValue,
              body[rootEnd] == Character("-").asciiValue else {
            return false
        }

        let rootName = body[..<rootEnd]
        guard !rootName.isEmpty,
              rootName.allSatisfy({
                ($0 >= Character("0").asciiValue! &&
                    $0 <= Character("9").asciiValue!) ||
                ($0 >= Character("A").asciiValue! &&
                    $0 <= Character("Z").asciiValue!) ||
                ($0 >= Character("a").asciiValue! &&
                    $0 <= Character("z").asciiValue!) ||
                $0 == Character("-").asciiValue! ||
                $0 == Character("_").asciiValue!
              }) else {
            return false
        }

        let timestamp = body[
            timestampStart..<(timestampStart + timestampLength)]
        guard timestamp[body.index(
                body.startIndex,
                offsetBy: timestampStart + 8)] ==
                Character("T").asciiValue,
              timestamp[body.index(
                body.startIndex,
                offsetBy: timestampStart + 15)] ==
                Character("Z").asciiValue else {
            return false
        }
        for index in 0..<timestampLength where index != 8 && index != 15 {
            let byte = body[timestampStart + index]
            guard byte >= Character("0").asciiValue!,
                  byte <= Character("9").asciiValue! else {
                return false
            }
        }

        return body[tokenStart...].allSatisfy {
            ($0 >= Character("0").asciiValue! &&
                $0 <= Character("9").asciiValue!) ||
            ($0 >= Character("a").asciiValue! &&
                $0 <= Character("f").asciiValue!)
        }
    }

    private static func identity(
        of metadata: stat,
        matches file: WatchSharedFile
    ) -> Bool {
        metadata.st_dev == file.deviceID &&
            metadata.st_ino == file.inodeNumber &&
            metadata.st_gen == file.generationNumber
    }

    private static func modificationDate(of metadata: stat) -> Date {
        Date(
            timeIntervalSince1970:
                TimeInterval(metadata.st_mtimespec.tv_sec) +
                TimeInterval(metadata.st_mtimespec.tv_nsec) /
                    1_000_000_000)
    }

    private static func safeJSONFileStem(_ preferredFileName: String) -> String {
        let value: Substring
        if preferredFileName.lowercased().hasSuffix(".json") {
            value = preferredFileName.dropLast(5)
        } else {
            value = preferredFileName[...]
        }

        var result = ""
        var lastWasSeparator = false
        for scalar in value.unicodeScalars {
            let isAllowed =
                CharacterSet.alphanumerics.contains(scalar) ||
                scalar == "-" || scalar == "_"
            if isAllowed {
                result.unicodeScalars.append(scalar)
                lastWasSeparator = false
            } else if !result.isEmpty && !lastWasSeparator {
                result.append("-")
                lastWasSeparator = true
            }
            if result.count >= 48 {
                break
            }
        }
        result = result.trimmingCharacters(
            in: CharacterSet(charactersIn: "-_"))
        return result.isEmpty ? "ish-theme" : result
    }
}

struct WatchSharedTransfer: Transferable, Sendable {
    let file: WatchSharedFile

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(exportedContentType: .data) { transfer in
            try sentTransferredFile(for: transfer.file)
        }
        .suggestedFileName { transfer in
            transfer.file.name
        }
    }

    static func sentTransferredFile(
        for file: WatchSharedFile
    ) throws -> SentTransferredFile {
        let directory = try WatchSharedFiles.ensureSystemDirectory()
        return try sentTransferredFile(for: file, in: directory)
    }

    static func sentTransferredFile(
        for file: WatchSharedFile,
        in directory: URL
    ) throws -> SentTransferredFile {
        try WatchSharedFiles.validate(file, in: directory)
        return SentTransferredFile(file.url)
    }
}

struct WatchRootArchiveRequest: Equatable, Sendable {
    static let excludedArchivePatterns = [
        "./proc/*",
        "proc/*",
        "./dev/pts/*",
        "dev/pts/*",
        "./mnt/shared/*",
        "mnt/shared/*",
        "./sys/*",
        "sys/*",
    ]

    let fileName: String
    let command: String

    init(
        rootName: String,
        date: Date = Date(),
        token: UUID = UUID()
    ) {
        let safeRootName = Self.safeFileNameComponent(rootName)
        let timestamp = Self.timestamp(date)
        let tokenText = token.uuidString
            .replacingOccurrences(of: "-", with: "")
            .lowercased()
        fileName =
            "ish-\(safeRootName)-\(timestamp)-\(tokenText).tar.gz"

        let archivePath =
            "\(WatchSharedFiles.guestMountPath)/\(fileName)"
        let partialPath =
            "\(WatchSharedFiles.guestMountPath)/.\(fileName).partial"
        let exclusions = Self.excludedArchivePatterns.map {
            "--exclude=\"\($0)\""
        }.joined(separator: " ")
        let body = """
        set -eu
        umask 077
        archive=\(Self.shellSingleQuoted(archivePath))
        partial=\(Self.shellSingleQuoted(partialPath))
        trap 'rm -f "$partial"' EXIT HUP INT TERM
        cd /
        printf '\\n正在导出文件系统…\\n'
        /bin/tar -czf "$partial" \(exclusions) .
        /bin/mv "$partial" "$archive"
        trap - EXIT HUP INT TERM
        """
        let quotedBody = Self.shellSingleQuoted(body)
        let quotedFileName = Self.shellSingleQuoted(fileName)
        command = """
        /bin/sh -c \(quotedBody)
        status=$?
        if [ "$status" -eq 0 ]; then
            printf '\\n导出完成：/mnt/shared/%s\\n' \(quotedFileName)
        else
            printf '\\n导出失败（状态 %s）\\n' "$status"
        fi
        exec /bin/login -f root
        """
    }

    private static func safeFileNameComponent(_ value: String) -> String {
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
        result = result.trimmingCharacters(
            in: CharacterSet(charactersIn: "-_"))
        return result.isEmpty ? "linux" : result
    }

    private static func timestamp(_ date: Date) -> String {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(secondsFromGMT: 0)!
        let components = calendar.dateComponents(
            [.year, .month, .day, .hour, .minute, .second],
            from: date)
        return String(
            format: "%04d%02d%02dT%02d%02d%02dZ",
            components.year ?? 0,
            components.month ?? 0,
            components.day ?? 0,
            components.hour ?? 0,
            components.minute ?? 0,
            components.second ?? 0)
    }

    private static func shellSingleQuoted(_ value: String) -> String {
        "'" + value.replacingOccurrences(
            of: "'", with: "'\"'\"'") + "'"
    }
}
