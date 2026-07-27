import Foundation
import SwiftUI

struct ThirdPartyNoticesView: View {
    @Environment(\.dismiss) private var dismiss

    private enum LoadState: Sendable {
        case loaded([String])
        case failed(String)
    }

    private static let linesPerChunk = 64
    private static let resourceNames = [
        "PROJECT-LICENSES",
        "THIRD-PARTY-NOTICES",
    ]
    private static let sourceURL = URL(
        string: "https://github.com/Eric-Terminal/ish-multiarch")!

    // watchOS 对单个超长 Text 的布局和辅助功能快照代价很高；
    // 分块后由 LazyVStack 只创建当前视口附近的正文。
    private static func chunks(for text: String) -> [String] {
        let lines = text.split(
            separator: "\n",
            omittingEmptySubsequences: false)
        return stride(
            from: 0,
            to: lines.count,
            by: linesPerChunk
        ).map { start in
            let end = min(start + linesPerChunk, lines.count)
            var chunk = lines[start..<end].joined(separator: "\n")
            if end < lines.count {
                chunk.append("\n")
            }
            return chunk
        }
    }

    // 声明正文只在首次打开查看页时读取，
    // 避免终端状态刷新触发重复 I/O。
    private static let loadState: LoadState = {
        var sections = [String]()
        for resourceName in resourceNames {
            guard let url = Bundle.main.url(
                    forResource: resourceName,
                    withExtension: "txt") else {
                return .failed("App 中缺少 \(resourceName).txt。")
            }
            guard let text = try? String(
                    contentsOf: url,
                    encoding: .utf8) else {
                return .failed(
                    "\(resourceName).txt 不是有效的 UTF-8 文本，" +
                    "或当前无法读取。")
            }
            sections.append(text)
        }
        return .loaded(chunks(for: sections.joined(separator: "\n\n")))
    }()

    var body: some View {
        NavigationStack {
            content
                .navigationTitle("许可证与源码")
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("完成") {
                            dismiss()
                        }
                        .accessibilityIdentifier(
                            "close-third-party-notices")
                    }
                }
        }
    }

    @ViewBuilder
    private var content: some View {
        switch Self.loadState {
        case let .loaded(chunks):
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    Link("查看源代码", destination: Self.sourceURL)
                        .padding(.bottom, 8)
                        .accessibilityIdentifier("project-source-link")
                    ForEach(chunks.indices, id: \.self) { index in
                        Text(verbatim: chunks[index])
                            .font(.system(
                                .caption2,
                                design: .monospaced))
                            .frame(
                                maxWidth: .infinity,
                                alignment: .topLeading)
                    }
                }
            }
            .padding(.horizontal, 4)
            .accessibilityIdentifier("third-party-notices-content")
        case let .failed(message):
            ScrollView {
                Text(verbatim: message)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .topLeading)
            }
            .padding(.horizontal, 4)
            .accessibilityIdentifier("third-party-notices-error")
        }
    }
}
