import Foundation

private func require(
    _ condition: @autoclosure () -> Bool,
    _ message: String
) {
    if !condition() {
        FileHandle.standardError.write(Data("失败：\(message)\n".utf8))
        exit(1)
    }
}

@main
private struct WatchHostRootArchiveTest {
    static func main() {
        require(
            WatchHostRootArchive.supportsImport(fileName: "root.tar"),
            "应接受 tar")
        require(
            WatchHostRootArchive.supportsImport(
                fileName: "root backup.tar.gz"),
            "应接受 tar.gz")
        require(
            WatchHostRootArchive.supportsImport(fileName: "ROOT.TGZ"),
            "应接受大小写不敏感的 tgz")
        for invalid in [
            "", ".", "..", "root.zip",
            "../root.tar", "folder/root.tar", "root.tar.gz/",
        ] {
            require(
                !WatchHostRootArchive.supportsImport(fileName: invalid),
                "不应接受 \(invalid)")
        }

        let date = Date(timeIntervalSince1970: 1_717_247_045)
        let token = UUID(
            uuidString: "11111111-2222-3333-4444-555555555555")!
        require(
            WatchHostRootArchive.exportFileName(
                rootName: "开发 / root",
                date: date,
                token: token) ==
                "ish-root-20240601-130405-" +
                "11111111222233334444555555555555.tar.gz",
            "导出名必须稳定、仅含安全 root 分量")
        require(
            WatchHostRootArchiveProgress(
                fraction: -1, message: "低").fraction == 0 &&
            WatchHostRootArchiveProgress(
                fraction: 2, message: "高").fraction == 1,
            "进度模型必须收敛到 0...1")
        print("Watch 宿主 Root 归档模型回归通过")
    }
}
