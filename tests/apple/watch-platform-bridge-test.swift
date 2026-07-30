import CoreFoundation
import Foundation

@main
struct WatchPlatformBridgeTest {
    private static var failures = 0

    static func main() {
        withDefaults { defaults in
            testWhitelist(defaults: defaults)
            testDefaultReads(defaults: defaults)
            testStringEnums(defaults: defaults)
            testFontSize(defaults: defaults)
            testBlinkCursor(defaults: defaults)
            testThemes(defaults: defaults)
            testCommands(defaults: defaults)
            testHostname(defaults: defaults)
            testRemovalAndUnknownKeys(defaults: defaults)
        }

        if failures == 0 {
            print("Watch /proc/ish 平台桥接回归通过")
        } else {
            fatalError("Watch /proc/ish 平台桥接回归失败：\(failures) 项")
        }
    }

    private static func withDefaults(
        _ body: (UserDefaults) -> Void
    ) {
        let suite = "ish.watch.platform.bridge.\(UUID().uuidString)"
        guard let defaults = UserDefaults(suiteName: suite) else {
            fatalError("无法创建隔离的 UserDefaults")
        }
        defer {
            defaults.removePersistentDomain(forName: suite)
        }
        body(defaults)
    }

    private static func store(
        defaults: UserDefaults
    ) -> WatchProcPreferenceStore {
        WatchProcPreferenceStore(
            defaults: defaults,
            automaticHostname: "automatic-watch")
    }

    private static func testWhitelist(defaults: UserDefaults) {
        let bridge = store(defaults: defaults)
        let expectedUnderlyingNames = [
            "watchTerminalFont",
            "watchTerminalFontSize",
            "watchTerminalPalette",
            "watchColorScheme",
            "watchTerminalCursorShape",
            "watchTerminalCursorBlink",
            "watchLaunchCommand",
            "watchBootCommand",
            "watchHostnameOverride",
        ]
        let expectedFriendlyNames = [
            "font_family",
            "font_size",
            "theme",
            "color_scheme",
            "cursor_style",
            "blink_cursor",
            "launch_command",
            "boot_command",
            "hostname_override",
        ]

        expect(
            WatchProcPreferenceStore.underlyingNames ==
                expectedUnderlyingNames,
            "defaults 白名单应恰好包含九个稳定键并保持顺序")
        expect(
            zip(expectedUnderlyingNames, expectedFriendlyNames).allSatisfy {
                bridge.friendlyName(for: $0.0) == $0.1 &&
                    bridge.underlyingName(for: $0.1) == $0.0
            },
            "九个 friendly/underlying 名称应双向一一映射")
        expect(
            bridge.friendlyName(for: "watchRootMetadata") == nil &&
                bridge.friendlyName(
                    for: WatchTerminalThemeStore.storageKey) == nil &&
                bridge.underlyingName(for: "copy_token") == nil,
            "桥接不能泄漏根文件系统、主题数据或复制状态等内部键")
    }

    private static func testDefaultReads(defaults: UserDefaults) {
        let bridge = store(defaults: defaults)
        let expected: [String: AnyHashable] = [
            "watchTerminalFont":
                WatchTerminalFontChoice.systemMonospaced.rawValue,
            "watchTerminalFontSize": 11,
            "watchTerminalPalette":
                WatchTerminalBuiltInThemeID.defaultTheme.rawValue,
            "watchColorScheme": "system",
            "watchTerminalCursorShape":
                WatchTerminalCursorShape.block.rawValue,
            "watchTerminalCursorBlink": false,
            "watchLaunchCommand":
                WatchPreferences.defaultLaunchCommand,
            "watchBootCommand":
                WatchPreferences.defaultBootCommand,
            "watchHostnameOverride": "automatic-watch",
        ]

        for name in WatchProcPreferenceStore.underlyingNames {
            guard let data = bridge.jsonData(for: name) else {
                expect(false, "\(name) 应可读取默认 JSON")
                continue
            }
            expect(
                data.last == 0x0a,
                "\(name) 的 proc 读取结果应以换行结尾")
            guard let value = try? JSONSerialization.jsonObject(
                with: data,
                options: .fragmentsAllowed) else {
                expect(false, "\(name) 应输出合法 JSON fragment")
                continue
            }
            expect(
                AnyHashable(value as! NSObject) == expected[name],
                "\(name) 应输出严格的默认值")
        }
        expect(
            bridge.jsonData(for: "watchCustomTerminalThemes") == nil,
            "未知或内部键不能读取")
    }

    private static func testStringEnums(defaults: UserDefaults) {
        let bridge = store(defaults: defaults)
        let cases = [
            (
                "watchTerminalFont",
                "\"rounded-monospaced\"",
                "rounded-monospaced"
            ),
            ("watchColorScheme", "\"dark\"", "dark"),
            ("watchTerminalCursorShape", "\"underline\"", "underline"),
        ]
        for (key, json, expected) in cases {
            expect(
                set(json, key: key, using: bridge),
                "\(key) 应接受白名单枚举值")
            expect(
                defaults.string(forKey: key) == expected,
                "\(key) 应保存枚举原始值")
            expect(
                !set("\"unknown\"", key: key, using: bridge) &&
                    defaults.string(forKey: key) == expected,
                "\(key) 应拒绝未知枚举且不修改旧值")
            expect(
                !set("1", key: key, using: bridge) &&
                    defaults.string(forKey: key) == expected,
                "\(key) 应拒绝错误 JSON 类型且不修改旧值")
        }
    }

    private static func testFontSize(defaults: UserDefaults) {
        let bridge = store(defaults: defaults)
        let key = "watchTerminalFontSize"

        expect(
            set("7", key: key, using: bridge) &&
                defaults.double(forKey: key) == 7,
            "字号应接受下边界 7")
        expect(
            set("24", key: key, using: bridge) &&
                defaults.double(forKey: key) == 24,
            "字号应接受上边界 24")
        for invalid in [
            "6", "25", "7.0", "11.5", "1e1",
            "7.0000000000000001", "24.000000000000001",
            "4294967303", "18446744073709551615",
            "1e300", "-1e300", "true", "\"11\"", "null",
        ] {
            expect(
                !set(invalid, key: key, using: bridge) &&
                    defaults.double(forKey: key) == 24,
                "字号应拒绝 \(invalid) 且保留旧值")
        }
    }

    private static func testBlinkCursor(defaults: UserDefaults) {
        let bridge = store(defaults: defaults)
        let key = "watchTerminalCursorBlink"

        expect(
            set("true", key: key, using: bridge) &&
                defaults.bool(forKey: key),
            "光标闪烁应接受 JSON boolean")
        for invalid in ["1", "\"true\"", "null", "[]"] {
            expect(
                !set(invalid, key: key, using: bridge) &&
                    defaults.bool(forKey: key),
                "光标闪烁应拒绝 \(invalid) 且保留旧值")
        }
    }

    private static func testThemes(defaults: UserDefaults) {
        let bridge = store(defaults: defaults)
        let key = "watchTerminalPalette"

        expect(
            set("\"solarized-light\"", key: key, using: bridge) &&
                defaults.string(forKey: key) ==
                    WatchTerminalBuiltInThemeID.solarized.rawValue,
            "兼容主题 ID 应规范化为逻辑主题选择 ID")

        let custom = WatchTerminalThemeStore.makeTheme(
            copyingBuiltIn: .classicGreen,
            existing: [])
        WatchTerminalThemeStore.save([custom], to: defaults)
        expect(
            set(
                jsonString(custom.selectionID),
                key: key,
                using: bridge) &&
                defaults.string(forKey: key) == custom.selectionID,
            "已保存自定义主题应只通过选择 ID 设置")
        guard let data = bridge.jsonData(for: key),
              let value = try? JSONSerialization.jsonObject(
                with: data,
                options: .fragmentsAllowed) as? String else {
            expect(false, "主题读取应返回 JSON 字符串")
            return
        }
        expect(
            value == custom.selectionID &&
                !String(decoding: data, as: UTF8.self)
                    .contains(custom.name),
            "主题读取只能返回选择 ID，不能泄漏自定义主题 Data")
        expect(
            !set(
                "\"custom:00000000-0000-0000-0000-000000000000\"",
                key: key,
                using: bridge) &&
                defaults.string(forKey: key) == custom.selectionID,
            "不存在的自定义主题 ID 应被拒绝且不修改旧值")
        expect(
            !set("{}", key: key, using: bridge),
            "主题不能通过对象写入自定义主题内容")
    }

    private static func testCommands(defaults: UserDefaults) {
        let bridge = store(defaults: defaults)
        let commands = [
            (
                "watchLaunchCommand",
                "  printf ready  ",
                "printf ready"
            ),
            (
                "watchBootCommand",
                "  exec /custom/init  ",
                "exec /custom/init"
            ),
        ]

        for (key, input, expected) in commands {
            expect(
                set(jsonString(input), key: key, using: bridge) &&
                    defaults.string(forKey: key) == expected,
                "\(key) 应接受 /bin/sh -c 使用的 JSON String 并去空白")
            expect(
                !set(
                    "[\"/bin/sh\",\"-c\",\"echo wrong\"]",
                    key: key,
                    using: bridge) &&
                    defaults.string(forKey: key) == expected,
                "\(key) 应拒绝 iOS argv 数组且不修改旧值")
            expect(
                !set("\"\"", key: key, using: bridge) &&
                    defaults.string(forKey: key) == expected,
                "\(key) 应拒绝空命令且不修改旧值")
            expect(
                !set("{", key: key, using: bridge) &&
                    defaults.string(forKey: key) == expected,
                "\(key) 应拒绝畸形 JSON 且不修改旧值")
        }
        expect(
            WatchPreferences.launchCommand(
                defaults.string(forKey: "watchLaunchCommand")) ==
                "printf ready",
            "guest 写入启动命令后，下一次会话刷新应立即看到新值")
    }

    private static func testHostname(defaults: UserDefaults) {
        let bridge = store(defaults: defaults)
        let key = "watchHostnameOverride"

        expect(
            set("\"  alpine-watch  \"", key: key, using: bridge) &&
                defaults.bool(
                    forKey:
                        WatchPreferenceKeys.customHostnameEnabled) &&
                defaults.string(
                    forKey: WatchPreferenceKeys.customHostname) ==
                    "alpine-watch",
            "主机名写入应规范化值并启用自定义主机名")
        expect(
            !set("\"watch\\nname\"", key: key, using: bridge) &&
                defaults.string(
                    forKey: WatchPreferenceKeys.customHostname) ==
                    "alpine-watch",
            "主机名应拒绝控制字符且不修改旧值")
        expect(
            bridge.removeValue(for: key) &&
                defaults.object(
                    forKey:
                        WatchPreferenceKeys.customHostnameEnabled) == nil &&
                defaults.object(
                    forKey: WatchPreferenceKeys.customHostname) == nil,
            "移除主机名应同时删除值和启用状态")
        expect(
            jsonStringValue(bridge.jsonData(for: key)) ==
                "automatic-watch",
            "移除主机名后应恢复自动主机名")

        defaults.set(
            "numeric-enabled",
            forKey: WatchPreferenceKeys.customHostname)
        defaults.set(
            1,
            forKey: WatchPreferenceKeys.customHostnameEnabled)
        expect(
            jsonStringValue(bridge.jsonData(for: key)) ==
                "automatic-watch",
            "损坏的数字启用状态不能冒充 JSON boolean")
    }

    private static func testRemovalAndUnknownKeys(
        defaults: UserDefaults
    ) {
        let bridge = store(defaults: defaults)
        defaults.set("dark", forKey: "watchColorScheme")
        expect(
            bridge.removeValue(for: "watchColorScheme") &&
                defaults.object(forKey: "watchColorScheme") == nil,
            "白名单键应允许移除")
        expect(
            !bridge.removeValue(for: "watchRootMetadata"),
            "未知或内部键不能移除")
        expect(
            !set(
                "\"secret\"",
                key: "watchRootMetadata",
                using: bridge),
            "未知或内部键不能写入")
    }

    private static func set(
        _ json: String,
        key: String,
        using store: WatchProcPreferenceStore
    ) -> Bool {
        store.setJSONData(
            Data(json.utf8),
            for: key)
    }

    private static func jsonString(_ string: String) -> String {
        let data = try! JSONSerialization.data(
            withJSONObject: string,
            options: .fragmentsAllowed)
        return String(decoding: data, as: UTF8.self)
    }

    private static func jsonStringValue(_ data: Data?) -> String? {
        guard let data else { return nil }
        return try? JSONSerialization.jsonObject(
            with: data,
            options: .fragmentsAllowed) as? String
    }

    private static func expect(
        _ condition: @autoclosure () -> Bool,
        _ message: String
    ) {
        if !condition() {
            print("失败：\(message)")
            failures += 1
        }
    }
}
