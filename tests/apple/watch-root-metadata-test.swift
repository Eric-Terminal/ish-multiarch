import Foundation

private func require(
    _ condition: @autoclosure () -> Bool,
    _ message: String
) {
    if !condition() {
        FileHandle.standardError.write(
            Data("失败：\(message)\n".utf8))
        exit(1)
    }
}

private func requireRenameError(
    _ expected: WatchRootMetadata.RenameError,
    operation: () throws -> Void
) {
    do {
        try operation()
        require(false, "重命名应当失败：\(expected)")
    } catch let error as WatchRootMetadata.RenameError {
        require(error == expected, "重命名错误不匹配")
    } catch {
        require(false, "重命名返回了意外错误：\(error)")
    }
}

@main
private struct WatchRootMetadataTest {
    static func main() {
        do {
            try testAliases()
            try testScheduledCopy()
            try testPersistenceShape()
            print("Watch 文件系统元数据回归通过")
        } catch {
            FileHandle.standardError.write(
                Data("失败：\(error)\n".utf8))
            exit(1)
        }
    }

    private static func testAliases() throws {
        var metadata = WatchRootMetadata()
        let roots = ["aarch64", "aarch64-2"]

        try metadata.rename(
            "aarch64", to: "日常环境", existingRootNames: roots)
        require(
            metadata.displayName(for: "aarch64") == "日常环境",
            "显示名称应与物理托管名称分离")
        require(
            metadata.displayName(for: "aarch64-2") == "aarch64-2",
            "未重命名环境应显示物理名称")

        requireRenameError(.duplicateName) {
            try metadata.rename(
                "aarch64-2", to: "日常环境", existingRootNames: roots)
        }
        requireRenameError(.emptyName) {
            try metadata.rename(
                "aarch64-2", to: " \n", existingRootNames: roots)
        }
        requireRenameError(.invalidCharacters) {
            try metadata.rename(
                "aarch64-2",
                to: "第一行\n第二行",
                existingRootNames: roots)
        }

        metadata.restoreDefaultName(
            for: "aarch64",
            existingRootNames: roots)
        require(
            metadata.displayName(for: "aarch64") == "aarch64",
            "恢复默认名称应删除别名")

        var createdCollision = WatchRootMetadata(
            aliases: ["aarch64": "aarch64-2"])
        createdCollision.reconcileAliases(
            existingRootNames: roots)
        require(
            Set(roots.map {
                createdCollision.displayName(for: $0).lowercased()
            }).count == roots.count,
            "新增物理名称不能与已有别名形成重复显示名称")

        var restoredCollision = WatchRootMetadata(
            aliases: [
                "aarch64": "工作环境",
                "aarch64-2": "aarch64",
            ])
        restoredCollision.reconcileAliases(
            existingRootNames: roots)
        restoredCollision.restoreDefaultName(
            for: "aarch64",
            existingRootNames: roots)
        require(
            Set(roots.map {
                restoredCollision.displayName(for: $0).lowercased()
            }).count == roots.count,
            "恢复物理名称后仍必须保持显示名称唯一")
    }

    private static func testScheduledCopy() throws {
        let requestToken = UUID(
            uuidString: "11111111-2222-3333-4444-555555555555")!
        var metadata = WatchRootMetadata(
            selectedRootName: "aarch64")
        let roots = ["aarch64", "aarch64-2"]
        try metadata.rename(
            "aarch64", to: "开发环境", existingRootNames: roots)
        try metadata.rename(
            "aarch64-2", to: "开发环境 副本", existingRootNames: roots)

        metadata.scheduleCopy(
            of: "aarch64",
            token: requestToken)
        require(
            metadata.pendingCopy?.sourceName == "aarch64" &&
                metadata.pendingCopy?.token == requestToken,
            "应记录下次启动前复制的来源")
        let request = metadata.pendingCopy!
        let staleRequest = WatchRootCopyRequest(
            token: UUID(),
            sourceName: "aarch64")
        require(
            !metadata.completeScheduledCopy(
                staleRequest,
                destinationName: "aarch64-3",
                existingRootNames: roots) &&
                metadata.pendingCopy == request,
            "过期的异步复制结果不能完成当前请求")
        require(
            metadata.completeScheduledCopy(
                request,
                destinationName: "aarch64-3",
                existingRootNames: roots),
            "匹配 token 的复制结果应完成当前请求")
        require(
            metadata.pendingCopy == nil &&
                metadata.selectedRootName == "aarch64-3",
            "复制完成后应原子清除待办并选择副本")
        require(
            metadata.displayName(for: "aarch64-3") ==
                "开发环境 副本 2",
            "复制别名应自动避开重复名称")

        metadata.scheduleCopy(of: "aarch64-3")
        metadata.select("aarch64-2")
        require(
            metadata.pendingCopy == nil &&
                metadata.selectedRootName == "aarch64-2",
            "用户后续明确选择其他环境时应取消旧复制计划")

        metadata.scheduleCopy(of: "aarch64-2")
        metadata.recordImmediateCopyAndSelect(
            from: "aarch64",
            to: "aarch64-4",
            existingRootNames: roots + ["aarch64-3"])
        require(
            metadata.pendingCopy == nil &&
                metadata.selectedRootName == "aarch64-4",
            "后续成功的复制并选择操作应成为唯一启动意图")

        metadata.scheduleCopy(of: "aarch64-4")
        metadata.remove("aarch64-3")
        require(
            metadata.pendingCopySource == "aarch64-4",
            "删除无关环境不能误取消复制待办")
        metadata.remove("aarch64-4")
        require(
            metadata.pendingCopy == nil,
            "删除环境时应同时清理其复制待办")
    }

    private static func testPersistenceShape() throws {
        let requestToken = UUID(
            uuidString: "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE")!
        var metadata = WatchRootMetadata(
            selectedRootName: "aarch64")
        try metadata.rename(
            "aarch64",
            to: "长期环境",
            existingRootNames: ["aarch64"])
        metadata.scheduleCopy(
            of: "aarch64",
            token: requestToken)

        let data = try JSONEncoder().encode(metadata)
        let decoded = try JSONDecoder().decode(
            WatchRootMetadata.self, from: data)
        require(decoded == metadata, "元数据应可无损持久化")
        require(
            decoded.selectedRootName == "aarch64" &&
                decoded.pendingCopy?.token == requestToken,
            "单一持久化值必须同时包含选择与复制事务")

        let suiteName =
            "ish-watch-root-metadata-test-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suiteName)!
        defer {
            defaults.removePersistentDomain(forName: suiteName)
        }
        WatchRootMetadataPersistence.save(metadata, to: defaults)
        require(
            defaults.data(
                forKey: WatchRootMetadataPersistence.storageKey) != nil,
            "生产持久化必须只写一个完整元数据值")
        require(
            WatchRootMetadataPersistence.load(
                from: defaults,
                legacySelectedRootName: nil) == metadata,
            "UserDefaults 实际保存和读取应无损")

        defaults.removeObject(
            forKey: WatchRootMetadataPersistence.storageKey)
        let migrated = WatchRootMetadataPersistence.load(
            from: defaults,
            legacySelectedRootName: "aarch64-9")
        require(
            migrated.selectedRootName == "aarch64-9",
            "首次升级应迁移旧的启动选择")

        var pruned = decoded
        pruned.removeMissingRoots([])
        require(pruned.aliases.isEmpty, "应移除不存在环境的别名")
        require(
            pruned.pendingCopySource == nil,
            "应移除不存在环境的复制待办")
    }
}
