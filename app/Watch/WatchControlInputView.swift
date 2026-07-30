#if os(watchOS)
import SwiftUI

struct WatchControlInputView: View {
    @ObservedObject var runtime: WatchRuntime
    @Binding var command: String

    private struct Key: Identifiable {
        let id: String
        let label: String
        let character: Character
    }

    private static let letters: [Key] =
        Array("ABCDEFGHIJKLMNOPQRSTUVWXYZ").map {
            Key(
                id: String($0).lowercased(),
                label: String($0),
                character: $0)
        }
    private static let symbols: [Key] = [
        Key(id: "at", label: "@", character: "@"),
        Key(id: "caret", label: "^", character: "^"),
        Key(id: "2", label: "2", character: "2"),
        Key(id: "6", label: "6", character: "6"),
        Key(id: "minus", label: "-", character: "-"),
        Key(id: "left-bracket", label: "[", character: "["),
        Key(id: "right-bracket", label: "]", character: "]"),
        Key(id: "backslash", label: "\\", character: "\\"),
        Key(id: "space", label: "Space", character: " "),
    ]
    private let letterColumns = Array(
        repeating: GridItem(.flexible(), spacing: 5),
        count: 4)
    private let symbolColumns = Array(
        repeating: GridItem(.flexible(), spacing: 5),
        count: 3)

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 10) {
                keyGrid(
                    "字母",
                    keys: Self.letters,
                    columns: letterColumns)
                keyGrid(
                    "符号",
                    keys: Self.symbols,
                    columns: symbolColumns)
            }
            .padding(.horizontal, 6)
            .padding(.bottom, 8)
        }
        .navigationTitle("Ctrl")
        .accessibilityIdentifier("watch-control-input-view")
    }

    private func keyGrid(
        _ title: String,
        keys: [Key],
        columns: [GridItem]
    ) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(title)
                .font(.caption2)
                .foregroundStyle(.secondary)
            LazyVGrid(columns: columns, spacing: 5) {
                ForEach(keys) { key in
                    Button {
                        if runtime.sendControl(
                            key.character,
                            after: command
                        ) {
                            command = ""
                        }
                    } label: {
                        Text(key.label)
                            .font(.caption2.monospaced().weight(.semibold))
                            .frame(maxWidth: .infinity, minHeight: 26)
                    }
                    .buttonStyle(.bordered)
                    .disabled(!runtime.acceptsInput)
                    .accessibilityLabel("Ctrl-\(key.label)")
                    .accessibilityIdentifier(
                        "send-full-control-\(key.id)")
                }
            }
        }
    }
}
#endif
