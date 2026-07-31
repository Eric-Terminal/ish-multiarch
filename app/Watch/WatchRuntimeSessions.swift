import Foundation

#if os(watchOS)
extension WatchRuntime {
    func scheduleSessionCreation(
        id: UUID,
        request: WatchTerminalSessionLaunchRequest
    ) {
        var generation: UInt64 = 0
        terminalSessions.update(id) {
            generation = $0.beginOperation()
        }
        sessionTasks[id] = Task { [weak self] in
            let result = await Task.detached(priority: .userInitiated) {
                Self.createCSession(
                    command: request.command,
                    columns: request.columns,
                    rows: request.rows)
            }.value
            guard let self else {
                if result.result >= 0 {
                    _ = await Task.detached {
                        ish_watch_session_close(result.id)
                    }.value
                }
                return
            }
            self.sessionTasks[id] = nil
            guard let current =
                    self.terminalSessions.session(id: id),
                  current.matchesOperation(
                    generation: generation) else {
                self.pendingClose.remove(id)
                if result.result >= 0 {
                    _ = await Task.detached {
                        ish_watch_session_close(result.id)
                    }.value
                }
                return
            }

            if self.pendingClose.remove(id) != nil {
                if result.result < 0 {
                    self.terminalSessions.remove(id)
                    self.refreshPublishedState()
                } else {
                    self.terminalSessions.update(id) {
                        $0.finishOpening(
                            sessionID: result.id,
                            createdColumns: request.columns,
                            createdRows: request.rows,
                            separatesPreviousOutput: false)
                        $0.lifecycle = .closing
                    }
                    self.scheduleSessionClose(
                        id: id, sessionID: result.id)
                }
                return
            }

            if result.result < 0 {
                self.terminalSessions.update(id) {
                    $0.markFailed(
                        WatchTerminalStatusText.errorMessage(
                        prefix: "终端启动失败",
                            code: result.result))
                }
                self.observeAPKUpgradeSession(id)
            } else {
                self.terminalSessions.update(id) {
                    $0.finishOpening(
                        sessionID: result.id,
                        createdColumns: request.columns,
                        createdRows: request.rows,
                        separatesPreviousOutput: false)
                }
            }
            self.refreshPublishedState()
        }
    }

    func scheduleSessionClose(id: UUID, sessionID: UInt64) {
        var generation: UInt64 = 0
        terminalSessions.update(id) {
            generation = $0.beginOperation()
            $0.lifecycle = .closing
        }
        refreshPublishedState()
        sessionTasks[id] = Task { [weak self] in
            let result = await Task.detached(priority: .userInitiated) {
                ish_watch_session_close(sessionID)
            }.value
            guard let self else { return }
            self.sessionTasks[id] = nil
            guard let current =
                    self.terminalSessions.session(id: id),
                  current.matchesOperation(
                    generation: generation,
                    sessionID: sessionID) else {
                self.pendingClose.remove(id)
                return
            }
            if result == 0 {
                self.terminalSessions.remove(id)
            } else {
                self.terminalSessions.update(id) {
                    $0.markFailed(
                        WatchTerminalStatusText.errorMessage(
                            prefix: "关闭终端失败", code: result),
                        sessionID: sessionID)
                }
            }
            self.refreshPublishedState()
        }
    }

    func finishPendingClose(
        id: UUID,
        createdResult: Int32,
        createdSessionID: UInt64,
        previousSessionID: UInt64,
        previousClosed: Bool
    ) {
        if createdResult >= 0 {
            terminalSessions.update(id) {
                $0.sessionID = createdSessionID
                $0.lifecycle = .closing
            }
            scheduleSessionClose(
                id: id, sessionID: createdSessionID)
        } else if !previousClosed && previousSessionID != 0 {
            terminalSessions.update(id) {
                $0.sessionID = previousSessionID
                $0.lifecycle = .closing
            }
            scheduleSessionClose(
                id: id, sessionID: previousSessionID)
        } else {
            terminalSessions.remove(id)
            refreshPublishedState()
        }
    }

    func poll() -> Bool {
        let phase = WatchGuestRuntimePhase(
            rawValue: ish_watch_runtime_current_phase())
        switch phase {
        case .idle:
            guestPhase = .idle
            recovery = nil
            globalStatusOverride = "准备中"
            refreshPublishedState()
            return true
        case .preparing:
            guestPhase = .preparing
            recovery = nil
            globalStatusOverride = "准备 Linux"
            refreshPublishedState()
            return true
        case .running:
            guestPhase = .running
            recovery = nil
            globalStatusOverride = nil
            let changed = pollSessions()
            if changed {
                refreshPublishedState()
            } else {
                refreshRuntimeAvailability()
            }
            return true
        case .stopped:
            guestPhase = .stopped
            recordAPKRepositoryRuntimeUnavailable()
            reportRecovery(
                detail: "Linux 已停止",
                recovery: .reopenApplication)
            refreshPublishedState()
            return false
        case .failed:
            guestPhase = .failed
            recordAPKRepositoryRuntimeUnavailable()
            reportRecovery(
                detail: WatchTerminalStatusText.errorMessage(
                    prefix: "运行失败",
                    code: ish_watch_runtime_last_error()),
                recovery: .reopenApplication)
            refreshPublishedState()
            return false
        case .none:
            guestPhase = .failed
            recordAPKRepositoryRuntimeUnavailable()
            reportRecovery(
                detail: "未知运行状态",
                recovery: .reopenApplication)
            refreshPublishedState()
            return false
        }
    }

    func pollSessions() -> Bool {
        pollTick &+= 1
        var changed = false
        if let activeID = terminalSessions.selectedSessionID {
            changed = pollSession(
                id: activeID, maximumReads: 4) || changed
        }

        // 非活动会话每秒左右轮询一个，避免多个空闲 PTY 常驻占用 Watch CPU。
        if pollTick % 5 == 0 {
            let activeID = terminalSessions.selectedSessionID
            let inactiveIDs = terminalSessions.sessions.compactMap {
                $0.id == activeID || $0.sessionID == 0 ? nil : $0.id
            }
            if !inactiveIDs.isEmpty {
                let id = inactiveIDs[
                    inactivePollIndex % inactiveIDs.count]
                inactivePollIndex =
                    (inactivePollIndex + 1) % inactiveIDs.count
                changed = pollSession(
                    id: id, maximumReads: 1) || changed
            }
        }
        return changed
    }

    func pollSession(
        id: UUID,
        maximumReads: Int
    ) -> Bool {
        guard let session = terminalSessions.session(id: id),
              session.sessionID != 0,
              session.lifecycle != .closing else {
            return false
        }
        if case .failed = session.lifecycle {
            return false
        }
        defer {
            observeAPKUpgradeSession(id)
        }
        let sessionID = session.sessionID
        var changed = drainOutput(
            id: id, maximumReads: maximumReads)
        if case .failed =
                terminalSessions.session(id: id)?.lifecycle {
            return true
        }

        var rawStatus = ish_watch_session_status()
        let statusResult = ish_watch_session_status(
            sessionID, &rawStatus)
        guard statusResult >= 0 else {
            terminalSessions.update(id) {
                $0.markFailed(
                    WatchTerminalStatusText.errorMessage(
                    prefix: "读取终端状态失败",
                    code: statusResult))
            }
            return true
        }

        switch WatchCSessionPhase(rawValue: rawStatus.phase) {
        case .starting:
            if terminalSessions.session(id: id)?.lifecycle != .opening {
                terminalSessions.update(id) { $0.lifecycle = .opening }
                changed = true
            }
        case .running:
            if terminalSessions.session(id: id)?.lifecycle != .running {
                terminalSessions.update(id) { $0.markRunning() }
                changed = true
            }
            changed = applyWindowSize(id: id) || changed
            changed = flushPendingInput(id: id) || changed
        case .exited:
            let description = WatchTerminalStatusText.exitDescription(
                rawStatus.wait_status)
            if terminalSessions.session(id: id)?.status != description {
                terminalSessions.update(id) {
                    $0.markExited(description)
                }
                changed = true
            }
        case .none:
            terminalSessions.update(id) {
                $0.markFailed("未知终端状态")
            }
            changed = true
        }
        return changed
    }

    func drainOutput(
        id: UUID,
        maximumReads: Int
    ) -> Bool {
        guard let sessionID =
                terminalSessions.session(id: id)?.sessionID,
              sessionID != 0 else {
            return false
        }
        var changed = false
        for _ in 0..<maximumReads {
            var dropped: UInt64 = 0
            let count = readBuffer.withUnsafeMutableBytes { rawBuffer in
                ish_watch_session_read_output(
                    sessionID,
                    rawBuffer.baseAddress,
                    rawBuffer.count,
                    &dropped)
            }
            if count < 0 {
                terminalSessions.update(id) {
                    $0.markFailed(
                        WatchTerminalStatusText.errorMessage(
                        prefix: "读取终端失败", code: count))
                }
                return true
            }
            if dropped != 0 {
                terminalSessions.update(id) {
                    $0.reportDroppedBytes(dropped)
                }
                changed = true
            }
            if count != 0 {
                let bytes = Array(readBuffer.prefix(count))
                terminalSessions.update(id) {
                    $0.appendOutput(bytes)
                }
                changed = true
            }
            if count < readBuffer.count {
                break
            }
        }
        return changed
    }

    @discardableResult
    func enqueue(_ bytes: [UInt8]) -> Bool {
        guard guestPhase == .running,
              let id = terminalSessions.selectedSessionID else {
            return false
        }
        var accepted = false
        terminalSessions.update(id) {
            accepted = $0.enqueue(bytes)
        }
        if accepted {
            _ = flushPendingInput(id: id)
        }
        refreshPublishedState()
        return accepted
    }

    @discardableResult
    func sendCursorKey(
        _ finalByte: UInt8,
        after pendingText: String = ""
    ) -> Bool {
        guard let session = terminalSessions.activeSession else {
            return false
        }
        return enqueue(WatchTerminalInput.sequence(
            session.cursorKey(finalByte),
            after: pendingText))
    }

    private func flushPendingInput(id: UUID) -> Bool {
        guard guestPhase == .running,
              let session = terminalSessions.session(id: id),
              session.acceptsInput else {
            return false
        }
        let sessionID = session.sessionID
        var changed = false
        // 限制部分写入次数，避免单一会话长时间占住 Watch 主线程。
        for _ in 0..<4 {
            guard let pending =
                    terminalSessions.session(id: id)?.pendingInput,
                  !pending.isEmpty else {
                break
            }
            let result = pending.withUnsafeBytes { rawBuffer in
                ish_watch_session_send_input(
                    sessionID,
                    rawBuffer.baseAddress,
                    rawBuffer.count)
            }
            if result > 0 {
                terminalSessions.update(id) {
                    $0.consumePendingInput(result)
                }
                changed = true
            } else if result == -11 || result == 0 {
                break
            } else {
                terminalSessions.update(id) {
                    $0.markFailed(
                        WatchTerminalStatusText.errorMessage(
                        prefix: "输入失败", code: result))
                }
                changed = true
                break
            }
        }
        return changed
    }

    func applyWindowSize(id: UUID) -> Bool {
        guard let session = terminalSessions.session(id: id),
              session.windowSizeNeedsUpdate,
              session.sessionID != 0 else {
            return false
        }
        let result = ish_watch_session_set_window_size(
            session.sessionID,
            UInt16(session.screen.columns),
            UInt16(session.screen.rows))
        if result == 0 {
            terminalSessions.update(id) {
                $0.windowSizeNeedsUpdate = false
            }
            return true
        }
        if result == -11 {
            return false
        }
        terminalSessions.update(id) {
            $0.markFailed(
                WatchTerminalStatusText.errorMessage(
                prefix: "调整终端尺寸失败", code: result))
        }
        return true
    }

}
#endif
