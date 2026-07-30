@main
struct WatchPreferencesTest {
    private static var failures = 0

    static func main() {
        expect(
            WatchPreferences.hostname(
                deviceName: "Eric 的 Apple Watch",
                usesCustomHostname: false,
                customHostname: "ignored") == "Eric 的 Apple Watch",
            "自动主机名应使用设备名称")
        expect(
            WatchPreferences.hostname(
                deviceName: "Watch",
                usesCustomHostname: true,
                customHostname: "  alpine-watch  ") == "alpine-watch",
            "自定义主机名应去掉首尾空白")
        expect(
            WatchPreferences.hostname(
                deviceName: "",
                usesCustomHostname: true,
                customHostname: " ") == "iSH",
            "空设备名和空自定义值应回落 iSH")

        let oversizedHostname = String(repeating: "晖", count: 22)
        expect(
            WatchPreferences.normalizedHostname(oversizedHostname) == nil,
            "主机名上限应按 UTF-8 字节计算")
        expect(
            WatchPreferences.normalizedHostname("watch\nname") == nil,
            "主机名应拒绝控制字符")
        expect(
            WatchPreferences.bootCommand(nil) ==
                WatchPreferences.defaultBootCommand,
            "缺失系统启动命令应使用默认 init")
        expect(
            WatchPreferences.bootCommand("  exec /custom/init  ") ==
                "exec /custom/init",
            "系统启动命令应去掉首尾空白")
        expect(
            WatchPreferences.launchCommand(nil) ==
                WatchPreferences.defaultLaunchCommand,
            "缺失终端启动命令应使用默认值")
        expect(
            WatchPreferences.launchCommand("  echo ready  ") ==
                "echo ready",
            "终端启动命令应去掉首尾空白")
        expect(
            WatchPreferences.launchCommand(
                String(repeating: "x", count: 4096)) ==
                WatchPreferences.defaultLaunchCommand,
            "过长终端启动命令应回落默认值")
        expect(
            WatchPreferences.launchCommand("echo\u{0}hidden") ==
                WatchPreferences.defaultLaunchCommand,
            "终端启动命令应拒绝会被 C 字符串截断的 NUL")
        expect(
            WatchPreferences.hostnameValidationIssue("") == .empty &&
                WatchPreferences.hostnameValidationIssue(
                    oversizedHostname) == .tooLong &&
                WatchPreferences.hostnameValidationIssue(
                    "watch\u{7}") == .controlCharacter,
            "主机名校验应区分空值、过长和控制字符")

        if failures == 0 {
            print("Watch 偏好设置回归通过")
        } else {
            fatalError("Watch 偏好设置回归失败：\(failures) 项")
        }
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
