import Foundation
import Darwin

@main
struct WatchSharedFilesTest {
    private static var failures = 0

    static func main() throws {
        let fileManager = FileManager.default
        let root = fileManager.temporaryDirectory.appendingPathComponent(
            "ish-watch-shared-files-\(UUID().uuidString)",
            isDirectory: true)
        defer {
            try? fileManager.removeItem(at: root)
        }
        try fileManager.createDirectory(
            at: root,
            withIntermediateDirectories: true)

        let shared = try WatchSharedFiles.ensureDirectory(
            in: root,
            fileManager: fileManager)
        expect(
            shared == root.appendingPathComponent(
                "Shared",
                isDirectory: true),
            "应在宿主 Documents 下创建固定 Shared 目录")
        try expect(
            try WatchSharedFiles.files(
                in: shared,
                fileManager: fileManager).isEmpty,
            "新建共享目录应显示明确的空列表")

        let first = shared.appendingPathComponent("01-first.txt")
        let second = shared.appendingPathComponent("02-second.txt")
        let hidden = shared.appendingPathComponent(".hidden")
        let directory = shared.appendingPathComponent(
            "nested",
            isDirectory: true)
        let outside = root.appendingPathComponent("outside.txt")
        try Data("second".utf8).write(to: second)
        try Data("first".utf8).write(to: first)
        try Data("hidden".utf8).write(to: hidden)
        try Data("outside".utf8).write(to: outside)
        try fileManager.createDirectory(
            at: directory,
            withIntermediateDirectories: false)
        let symlink = shared.appendingPathComponent("linked.txt")
        try fileManager.createSymbolicLink(
            at: symlink,
            withDestinationURL: outside)

        var files = try WatchSharedFiles.files(
            in: shared,
            fileManager: fileManager)
        expect(
            files.map(\.name) == [
                "01-first.txt",
                "02-second.txt",
            ],
            "只应稳定列出顶层、非隐藏、非符号链接的普通文件")
        expect(
            files.map(\.byteCount) == [5, 6],
            "共享文件条目应包含准确且非负的文件大小")

        guard let firstEntry = files.first else {
            fatalError("缺少待删除的普通文件条目")
        }
        var firstMetadata = stat()
        expect(
            lstat(first.path, &firstMetadata) == 0 &&
                firstEntry.deviceID == firstMetadata.st_dev &&
                firstEntry.inodeNumber == firstMetadata.st_ino &&
                firstEntry.generationNumber == firstMetadata.st_gen,
            "共享文件条目应保存枚举时的完整宿主文件身份")
        try expect(
            try WatchSharedFiles.read(
                firstEntry,
                from: shared,
                maximumByteCount: 5) == Data("first".utf8),
            "应通过稳定普通文件描述符读取大小受限的共享文件")

        let sparse = shared.appendingPathComponent("03-sparse.bin")
        expect(
            fileManager.createFile(
                atPath: sparse.path,
                contents: nil),
            "应能创建大文件分享回归样本")
        let sparseHandle = try FileHandle(forWritingTo: sparse)
        try sparseHandle.truncate(atOffset: 2 * 1024 * 1024 * 1024)
        try sparseHandle.close()
        let sparseEntry = try WatchSharedFiles.files(
            in: shared,
            fileManager: fileManager
        ).first { $0.name == sparse.lastPathComponent }!
        let sparseTransfer = try WatchSharedTransfer.sentTransferredFile(
            for: sparseEntry,
            in: shared)
        expect(
            sparseTransfer.file.standardizedFileURL ==
                sparse.standardizedFileURL &&
                !sparseTransfer.allowAccessingOriginalFile,
            "大文件分享应交给文件传输表示，不得整体载入 Watch 内存")
        try fileManager.removeItem(at: sparse)

        expectThrows(
            WatchSharedFilesError.fileTooLarge,
            "共享文件超过操作上限时必须在读取前拒绝"
        ) {
            _ = try WatchSharedFiles.read(
                firstEntry,
                from: shared,
                maximumByteCount: 4)
        }
        try WatchSharedFiles.delete(
            firstEntry,
            from: shared,
            fileManager: fileManager)
        expect(
            !fileManager.fileExists(atPath: first.path),
            "应允许删除 Shared 顶层的普通文件")

        let replacedFile =
            shared.appendingPathComponent("03-replaced.txt")
        try Data("original".utf8).write(to: replacedFile)
        let replacedFileEntry = try WatchSharedFiles.files(
            in: shared,
            fileManager: fileManager
        ).first { $0.name == replacedFile.lastPathComponent }!
        try fileManager.removeItem(at: replacedFile)
        try Data("replacement".utf8).write(to: replacedFile)
        expectThrows(
            WatchSharedFilesError.invalidFile,
            "读取时不能把同名替换文件当作枚举时的原文件"
        ) {
            _ = try WatchSharedFiles.read(
                replacedFileEntry,
                from: shared,
                maximumByteCount: 64)
        }
        expectThrows(
            WatchSharedFilesError.invalidFile,
            "分享时不能把同名替换文件当作枚举时的原文件"
        ) {
            _ = try WatchSharedTransfer.sentTransferredFile(
                for: replacedFileEntry,
                in: shared)
        }
        expectThrows(
            WatchSharedFilesError.fileChanged,
            "删除前出现同名替换文件时必须拒绝旧条目"
        ) {
            try WatchSharedFiles.delete(
                replacedFileEntry,
                from: shared,
                fileManager: fileManager)
        }
        try expect(
            try Data(contentsOf: replacedFile) ==
                Data("replacement".utf8),
            "拒绝旧条目后必须把同名替换文件原样恢复")
        try fileManager.removeItem(at: replacedFile)

        let replacedDirectory =
            shared.appendingPathComponent("04-replaced-directory")
        try Data("original".utf8).write(to: replacedDirectory)
        let replacedDirectoryEntry = try WatchSharedFiles.files(
            in: shared,
            fileManager: fileManager
        ).first { $0.name == replacedDirectory.lastPathComponent }!
        try fileManager.removeItem(at: replacedDirectory)
        try fileManager.createDirectory(
            at: replacedDirectory,
            withIntermediateDirectories: false)
        let directorySentinel =
            replacedDirectory.appendingPathComponent("sentinel.txt")
        try Data("keep".utf8).write(to: directorySentinel)
        expectThrows(
            WatchSharedFilesError.fileChanged,
            "同名替换成目录时必须拒绝删除"
        ) {
            try WatchSharedFiles.delete(
                replacedDirectoryEntry,
                from: shared,
                fileManager: fileManager)
        }
        expect(
            fileManager.fileExists(atPath: directorySentinel.path),
            "拒绝同名目录后不得递归删除其中的内容")
        try fileManager.removeItem(at: replacedDirectory)

        let replacedLink =
            shared.appendingPathComponent("05-replaced-link.txt")
        try Data("original".utf8).write(to: replacedLink)
        let replacedLinkEntry = try WatchSharedFiles.files(
            in: shared,
            fileManager: fileManager
        ).first { $0.name == replacedLink.lastPathComponent }!
        try fileManager.removeItem(at: replacedLink)
        try fileManager.createSymbolicLink(
            at: replacedLink,
            withDestinationURL: outside)
        expectThrows(
            WatchSharedFilesError.fileChanged,
            "同名替换成符号链接时必须拒绝删除"
        ) {
            try WatchSharedFiles.delete(
                replacedLinkEntry,
                from: shared,
                fileManager: fileManager)
        }
        try expect(
            try Data(contentsOf: outside) == Data("outside".utf8),
            "拒绝同名符号链接后不得改动链接目标")
        try fileManager.removeItem(at: replacedLink)

        let missing = shared.appendingPathComponent("06-missing.txt")
        try Data("gone".utf8).write(to: missing)
        let missingEntry = try WatchSharedFiles.files(
            in: shared,
            fileManager: fileManager
        ).first { $0.name == missing.lastPathComponent }!
        try fileManager.removeItem(at: missing)
        expectThrows(
            WatchSharedFilesError.fileChanged,
            "枚举后的文件消失时必须报告条目已变化"
        ) {
            try WatchSharedFiles.delete(
                missingEntry,
                from: shared,
                fileManager: fileManager)
        }
        let pendingDeletes = try fileManager.contentsOfDirectory(
            at: shared,
            includingPropertiesForKeys: nil)
            .filter {
                $0.lastPathComponent.hasPrefix(".ish-delete-") ||
                    $0.lastPathComponent.hasPrefix(
                        "ish-delete-recovery-")
            }
        expect(
            pendingDeletes.isEmpty,
            "身份不符并成功回滚后不得留下删除暂存项")

        let forgedOutside = WatchSharedFile(
            url: outside,
            name: outside.lastPathComponent,
            byteCount: 7,
            modificationDate: nil,
            deviceID: 0,
            inodeNumber: 0,
            generationNumber: 0)
        expectThrowsInvalidFile(
            "不能删除 Shared 之外的文件"
        ) {
            try WatchSharedFiles.delete(
                forgedOutside,
                from: shared,
                fileManager: fileManager)
        }

        let forgedSymlink = WatchSharedFile(
            url: symlink,
            name: symlink.lastPathComponent,
            byteCount: 7,
            modificationDate: nil,
            deviceID: 0,
            inodeNumber: 0,
            generationNumber: 0)
        expectThrows(
            WatchSharedFilesError.cannotReadFile,
            "不能通过符号链接读取宿主文件"
        ) {
            _ = try WatchSharedFiles.read(
                forgedSymlink,
                from: shared,
                maximumByteCount: 64)
        }
        expectThrows(
            WatchSharedFilesError.fileChanged,
            "不能通过符号链接删除宿主文件"
        ) {
            try WatchSharedFiles.delete(
                forgedSymlink,
                from: shared,
                fileManager: fileManager)
        }

        let forgedDirectory = WatchSharedFile(
            url: directory,
            name: directory.lastPathComponent,
            byteCount: 0,
            modificationDate: nil,
            deviceID: 0,
            inodeNumber: 0,
            generationNumber: 0)
        expectThrows(
            WatchSharedFilesError.fileChanged,
            "不能把子目录当作普通共享文件删除"
        ) {
            try WatchSharedFiles.delete(
                forgedDirectory,
                from: shared,
                fileManager: fileManager)
        }

        let exportedData = Data(
            #"{"version":1,"shared":{}}"#.utf8)
        let exported = try WatchSharedFiles.write(
            exportedData,
            preferredFileName: "../主题/坏?.json",
            to: shared)
        let collidingExport = try WatchSharedFiles.write(
            Data("second-export".utf8),
            preferredFileName: "../主题/坏?.json",
            to: shared)
        expect(
            exported.name == "主题-坏.json" &&
                collidingExport.name == "主题-坏 2.json" &&
                exported.url.deletingLastPathComponent() == shared &&
                collidingExport.url.deletingLastPathComponent() == shared,
            "共享写入应清理路径字符，并为同名主题自动选择唯一文件名")
        try expect(
            try Data(contentsOf: exported.url) == exportedData &&
                Data(contentsOf: collidingExport.url) ==
                    Data("second-export".utf8),
            "命名冲突不得覆盖既有文件，两次导出内容都应保留")
        let partialFiles = try fileManager.contentsOfDirectory(
            at: shared,
            includingPropertiesForKeys: nil)
            .filter { $0.lastPathComponent.hasSuffix(".partial") }
        expect(
            partialFiles.isEmpty,
            "成功原子发布后不得残留隐藏的 partial 文件")

        let unwritable = root.appendingPathComponent(
            "Unwritable",
            isDirectory: true)
        try fileManager.createDirectory(
            at: unwritable,
            withIntermediateDirectories: false)
        try fileManager.setAttributes(
            [.posixPermissions: 0o500],
            ofItemAtPath: unwritable.path)
        expectThrows(
            WatchSharedFilesError.cannotWriteFile,
            "共享写入失败时应返回明确错误"
        ) {
            _ = try WatchSharedFiles.write(
                exportedData,
                preferredFileName: "失败主题",
                to: unwritable)
        }
        try fileManager.setAttributes(
            [.posixPermissions: 0o700],
            ofItemAtPath: unwritable.path)
        let failureResidue = try fileManager.contentsOfDirectory(
            at: unwritable,
            includingPropertiesForKeys: nil)
        expect(
            failureResidue.isEmpty,
            "写入失败后不得留下 partial 或目标文件")

        let sharedLink = root.appendingPathComponent(
            "Shared-Link",
            isDirectory: true)
        try fileManager.createSymbolicLink(
            at: sharedLink,
            withDestinationURL: shared)
        expectThrows(
            WatchSharedFilesError.cannotWriteFile,
            "共享写入不能跟随替代目录的符号链接"
        ) {
            _ = try WatchSharedFiles.write(
                exportedData,
                preferredFileName: "符号链接",
                to: sharedLink)
        }

        try fileManager.removeItem(at: second)
        try fileManager.removeItem(at: exported.url)
        try fileManager.removeItem(at: collidingExport.url)
        files = try WatchSharedFiles.files(
            in: shared,
            fileManager: fileManager)
        expect(
            files.isEmpty,
            "文件在刷新前消失时，下次刷新应回到空状态")
        expect(
            WatchSharedFiles.guestMountPath == "/mnt/shared" &&
                WatchSharedFiles.terminalCommand ==
                    "cd /mnt/shared && exec /bin/sh -l" &&
                !WatchSharedFiles.terminalCommand.contains("mount") &&
                !WatchSharedFiles.terminalCommand.contains(
                    "/proc/ish/documents"),
            "共享终端只能进入 C 运行时预挂载的固定路径")

        let interrupted = root.appendingPathComponent(
            "Interrupted",
            isDirectory: true)
        try fileManager.createDirectory(
            at: interrupted,
            withIntermediateDirectories: false)
        let staleName =
            ".ish-AArch64_1-19700101T000000Z-" +
            "0123456789abcdef0123456789abcdef.tar.gz.partial"
        let stale = interrupted.appendingPathComponent(staleName)
        try Data("partial".utf8).write(to: stale)

        let nearMissNames = [
            String(staleName.dropFirst()),
            ".ish-AArch64_1-19700101T000000Z-" +
                "1123456789ABCDEF1123456789ABCDEF.tar.gz.partial",
            ".ish-AArch64_1-19700101X000000Z-" +
                "1123456789abcdef0123456789abcdef.tar.gz.partial",
            ".ish-theme-01234567-89ab-cdef-0123-" +
                "456789abcdef.partial",
            "ish-visible.tar.gz",
        ]
        for name in nearMissNames {
            try Data("保留".utf8).write(
                to: interrupted.appendingPathComponent(name))
        }

        let linkedPartial = interrupted.appendingPathComponent(
            ".ish-linked-19700101T000000Z-" +
                "2123456789abcdef0123456789abcdef.tar.gz.partial")
        try fileManager.createSymbolicLink(
            at: linkedPartial,
            withDestinationURL: outside)
        let directoryPartial = interrupted.appendingPathComponent(
            ".ish-directory-19700101T000000Z-" +
                "3123456789abcdef0123456789abcdef.tar.gz.partial",
            isDirectory: true)
        try fileManager.createDirectory(
            at: directoryPartial,
            withIntermediateDirectories: false)

        let removedPartials =
            try WatchSharedFiles.cleanupInterruptedRootArchivePartials(
                in: interrupted)
        expect(
            removedPartials == 1 &&
                !fileManager.fileExists(atPath: stale.path),
            "冷启动只能删除严格匹配且为普通文件的中断 Root 归档")
        for name in nearMissNames {
            expect(
                fileManager.fileExists(
                    atPath: interrupted
                        .appendingPathComponent(name).path),
                "冷启动不得删除近似名称：\(name)")
        }
        expect(
            fileManager.fileExists(atPath: linkedPartial.path) &&
                fileManager.fileExists(atPath: directoryPartial.path),
            "冷启动不得跟随符号链接或删除同名目录")
        try expect(
            try WatchSharedFiles
                .cleanupInterruptedRootArchivePartials(
                    in: interrupted) == 0,
            "重复清理中断归档必须幂等")

        let crowded = root.appendingPathComponent(
            "Crowded",
            isDirectory: true)
        try fileManager.createDirectory(
            at: crowded,
            withIntermediateDirectories: false)
        for index in 0...WatchSharedFiles.maximumDirectoryEntryCount {
            let path = crowded.appendingPathComponent(
                String(format: "%04d", index))
            expect(
                fileManager.createFile(
                    atPath: path.path,
                    contents: nil),
                "应能创建目录数量上限回归样本")
        }
        expectThrows(
            WatchSharedFilesError.tooManyFiles,
            "共享目录超过显示上限时必须及时停止枚举"
        ) {
            _ = try WatchSharedFiles.files(
                in: crowded,
                fileManager: fileManager)
        }

        testRootArchiveRequest()

        if failures == 0 {
            print("Watch 共享文件模型回归通过")
        } else {
            fatalError("Watch 共享文件模型回归失败：\(failures) 项")
        }
    }

    private static func testRootArchiveRequest() {
        let token = UUID(
            uuidString: "01234567-89AB-CDEF-0123-456789ABCDEF")!
        let request = WatchRootArchiveRequest(
            rootName: "../AArch64 '测试'/\n",
            date: Date(timeIntervalSince1970: 0),
            token: token)

        expect(
            request.fileName ==
                "ish-AArch64-19700101T000000Z-" +
                "0123456789abcdef0123456789abcdef.tar.gz",
            "归档文件名应移除路径、引号、控制字符和非 ASCII 字符")
        expect(
            !request.fileName.contains("/") &&
                !request.fileName.contains("..") &&
                !request.fileName.contains("'") &&
                request.fileName.hasSuffix(".tar.gz"),
            "归档文件名必须是 Shared 顶层的安全普通文件名")
        expect(
            WatchRootArchiveRequest.excludedArchivePatterns == [
                "./proc/*",
                "proc/*",
                "./dev/pts/*",
                "dev/pts/*",
                "./mnt/shared/*",
                "mnt/shared/*",
                "./sys/*",
                "sys/*",
            ],
            "归档应兼容 BusyBox 成员名并排除动态挂载内容")
        for pattern in
                WatchRootArchiveRequest.excludedArchivePatterns {
            expect(
                request.command.contains(
                    "--exclude=\\'\(pattern)\\'") ||
                    request.command.contains(
                        "--exclude=\"\(pattern)\""),
                "归档命令缺少排除规则 \(pattern)")
        }
        expect(
            request.command.contains(
                "/bin/tar -czf \"$partial\"") &&
                request.command.contains(
                    "--exclude=\"./mnt/shared/*\"") &&
                request.command.contains(
                    "--exclude=\"mnt/shared/*\"") &&
                request.command.contains(
                    "/bin/mv \"$partial\" \"$archive\""),
            "归档必须排除自身并在同一 Shared 目录原子发布")
        expect(
            request.command.contains("rm -f \"$partial\"") &&
                request.command.contains("EXIT HUP INT TERM"),
            "失败、中断或退出时必须清理未完成归档")
        expect(
            request.command.contains("正在导出文件系统") &&
                request.command.contains("导出完成：/mnt/shared/%s") &&
                request.command.contains("导出失败（状态 %s）"),
            "导出终端必须提供明确的开始、成功和失败反馈")
        if let progress = request.command.range(
                of: "正在导出文件系统"),
           let archive = request.command.range(
                of: "/bin/tar -czf \"$partial\"") {
            expect(
                progress.lowerBound < archive.lowerBound,
                "开始反馈必须在静默归档命令之前输出")
        } else {
            expect(false, "无法定位导出开始反馈或归档命令")
        }
        expect(
            !request.command.contains("libarchive") &&
                !request.command.contains(
                    "/proc/ish/documents"),
            "归档只能经 guest tar 与现有共享挂载完成")
    }

    private static func expectThrowsInvalidFile(
        _ message: String,
        operation: () throws -> Void
    ) {
        do {
            try operation()
            expect(false, message)
        } catch WatchSharedFilesError.invalidFile {
            return
        } catch {
            expect(false, "\(message)：错误类型不正确 \(error)")
        }
    }

    private static func expectThrows(
        _ expected: WatchSharedFilesError,
        _ message: String,
        operation: () throws -> Void
    ) {
        do {
            try operation()
            expect(false, message)
        } catch let error as WatchSharedFilesError {
            expect(error == expected, "\(message)：错误类型不正确 \(error)")
        } catch {
            expect(false, "\(message)：错误类型不正确 \(error)")
        }
    }

    private static func expect(
        _ condition: @autoclosure () throws -> Bool,
        _ message: String
    ) rethrows {
        if try !condition() {
            print("失败：\(message)")
            failures += 1
        }
    }
}
