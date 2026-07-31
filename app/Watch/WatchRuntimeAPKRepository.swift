import Foundation

#if os(watchOS)
extension WatchRuntime {
    func scheduleAPKRepositoryInspection() {
        guard apkRepositoryTask == nil else { return }
        updateAPKRepositoryMigration {
            $0.beginChecking()
        }
        let token = UUID()
        apkRepositoryTaskToken = token
        apkRepositoryTask = Task { [weak self] in
            let result = await Task.detached(priority: .utility) {
                Self.inspectAPKRepositories()
            }.value
            guard let self,
                  self.apkRepositoryTaskToken == token else {
                return
            }
            self.apkRepositoryTask = nil
            self.apkRepositoryTaskToken = nil
            if let status = result.status {
                self.updateAPKRepositoryMigration {
                    $0.recordInspection(status)
                }
            }
            if let errorMessage = result.errorMessage {
                self.updateAPKRepositoryMigration {
                    $0.recordFailure(
                        errorMessage,
                        preserving: result.status)
                }
            }
        }
    }

    nonisolated private static func inspectAPKRepositories()
        -> APKRepositoryInspectionResult
    {
        let service = WatchAPKRepositoryMigrationService(
            guestFiles: WatchAPKRepositoryGuestFileStore())
        let status: WatchAPKRepositoryMigrationStatus
        do {
            status = try service.status()
        } catch {
            return APKRepositoryInspectionResult(
                status: nil,
                errorMessage: error.localizedDescription)
        }

        guard case .current = status else {
            return APKRepositoryInspectionResult(
                status: status,
                errorMessage: nil)
        }
        do {
            let template =
                try WatchAPKRepositoriesTemplate.bundled()
            _ = try service.prepareRepositories(template: template)
            return APKRepositoryInspectionResult(
                status: status,
                errorMessage: nil)
        } catch {
            return APKRepositoryInspectionResult(
                status: status,
                errorMessage: error.localizedDescription)
        }
    }

    nonisolated static func prepareAPKRepositoriesForUpgrade()
        -> APKRepositoryOperationResult
    {
        do {
            let service = WatchAPKRepositoryMigrationService(
                guestFiles: WatchAPKRepositoryGuestFileStore())
            let status = try service.status()
            if case .newerThanApp = status {
                return .success(.skipped(status))
            }
            let template =
                try WatchAPKRepositoriesTemplate.bundled()
            return .success(
                try service.prepareRepositories(template: template))
        } catch {
            return .failure(error.localizedDescription)
        }
    }

    nonisolated private static func finishAPKRepositoryMigration()
        -> APKRepositoryOperationResult
    {
        do {
            let service = WatchAPKRepositoryMigrationService(
                guestFiles: WatchAPKRepositoryGuestFileStore())
            let status = try service.status()
            if case .newerThanApp = status {
                return .success(.skipped(status))
            }
            let template =
                try WatchAPKRepositoriesTemplate.bundled()
            return .success(
                try service.finishMigration(template: template))
        } catch {
            return .failure(error.localizedDescription)
        }
    }

    func observeAPKUpgradeSession(_ id: UUID) {
        guard let session = terminalSessions.session(id: id),
              session.purpose ==
                .softwareMaintenance(.upgradePackages),
              let phase = session.maintenancePhase else {
            return
        }
        let upgradePhase:
            WatchAPKRepositoryMigrationCoordinator.UpgradePhase
        switch phase {
        case .executing:
            upgradePhase = .executing
        case .completed:
            upgradePhase = .completed
        case let .failed(status):
            upgradePhase = .failed(status)
        case .interrupted:
            upgradePhase = .interrupted
        }

        var migration = apkRepositoryMigration
        let shouldFinish = migration.observeMaintenance(
            sessionID: id,
            phase: upgradePhase)
        if migration != apkRepositoryMigration {
            apkRepositoryMigration = migration
        }
        if shouldFinish {
            scheduleAPKRepositoryMigrationFinish(sessionID: id)
        }
    }

    func recordAPKUpgradeSessionClosed(_ id: UUID) {
        updateAPKRepositoryMigration {
            $0.recordSessionClosed(id)
        }
    }

    private func scheduleAPKRepositoryMigrationFinish(
        sessionID: UUID
    ) {
        guard apkRepositoryTask == nil else {
            updateAPKRepositoryMigration {
                $0.recordMigrationFinishFailure(
                    sessionID: sessionID,
                    message: "迁移收尾任务冲突，请稍后重试软件升级")
            }
            return
        }
        let token = UUID()
        apkRepositoryTaskToken = token
        apkRepositoryTask = Task { [weak self] in
            let result = await Task.detached(priority: .userInitiated) {
                Self.finishAPKRepositoryMigration()
            }.value
            guard let self,
                  self.apkRepositoryTaskToken == token else {
                return
            }
            self.apkRepositoryTask = nil
            self.apkRepositoryTaskToken = nil
            switch result {
            case let .success(outcome):
                self.updateAPKRepositoryMigration {
                    $0.recordMigrationFinished(
                        sessionID: sessionID,
                        outcome: outcome)
                }
            case let .failure(message):
                self.updateAPKRepositoryMigration {
                    $0.recordMigrationFinishFailure(
                        sessionID: sessionID,
                        message: "记录软件仓库迁移状态失败：\(message)")
                }
            }
        }
    }

    func updateAPKRepositoryMigration(
        _ update: (inout WatchAPKRepositoryMigrationCoordinator) -> Void
    ) {
        var migration = apkRepositoryMigration
        update(&migration)
        if migration != apkRepositoryMigration {
            apkRepositoryMigration = migration
        }
    }

    func recordAPKRepositoryRuntimeUnavailable() {
        updateAPKRepositoryMigration {
            $0.recordRuntimeUnavailable(
                "Linux 未运行，无法检查")
        }
    }
}
#endif
