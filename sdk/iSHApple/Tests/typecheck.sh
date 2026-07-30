#!/bin/zsh
set -euo pipefail

repository_root=${0:A:h:h:h:h}
headers=${1:-"$repository_root/sdk/iSHApple/Headers"}
if [[ ! -f "$headers/iSHApple.h" ||
      ! -f "$headers/module.modulemap" ]]; then
    print -u2 "错误：Swift 公共包装找不到完整的 iSHApple Headers：$headers"
    exit 1
fi
sources=("$repository_root"/sdk/iSHApple/Sources/iSHAppleSwift/*.swift)
consumer="$repository_root/examples/iSHAppleCommandConsumer/main.swift"
compile_test="$repository_root/sdk/iSHApple/Tests/PublicAPICompileTest.swift"
c_string_test="$repository_root/sdk/iSHApple/Tests/CStringStorageTest.swift"
channel_test="$repository_root/sdk/iSHApple/Tests/BoundedOutputChannelTest.swift"
temporary_directory=$(mktemp -d /private/tmp/ish-apple-swift.XXXXXX)
trap 'rm -rf "$temporary_directory"' EXIT

common_flags=(
    -swift-version 6
    -strict-concurrency=complete
    -warnings-as-errors
    -parse-as-library
    -I "$headers"
)

xcrun swiftc \
    "${common_flags[@]}" \
    -emit-module \
    -module-name iSHAppleSwift \
    -emit-module-path "$temporary_directory/iSHAppleSwift.swiftmodule" \
    "${sources[@]}"

xcrun swiftc \
    "${common_flags[@]}" \
    -I "$temporary_directory" \
    -typecheck \
    "$compile_test"

xcrun swiftc \
    "${common_flags[@]}" \
    -I "$temporary_directory" \
    -typecheck \
    "$consumer"

xcrun swiftc \
    -swift-version 6 \
    -strict-concurrency=complete \
    -warnings-as-errors \
    -parse-as-library \
    "$repository_root/sdk/iSHApple/Sources/iSHAppleSwift/CStringStorage.swift" \
    "$c_string_test" \
    -o "$temporary_directory/c-string-storage-test"
"$temporary_directory/c-string-storage-test"

xcrun swiftc \
    -swift-version 6 \
    -strict-concurrency=complete \
    -warnings-as-errors \
    -parse-as-library \
    -I "$headers" \
    "$repository_root/sdk/iSHApple/Sources/iSHAppleSwift/BridgeError.swift" \
    "$repository_root/sdk/iSHApple/Sources/iSHAppleSwift/CommandTypes.swift" \
    "$repository_root/sdk/iSHApple/Sources/iSHAppleSwift/BoundedOutputChannel.swift" \
    "$channel_test" \
    -o "$temporary_directory/bounded-output-channel-test"
"$temporary_directory/bounded-output-channel-test"

for platform in \
    "iphoneos arm64-apple-ios15.0 ios-arm64.swiftmodule" \
    "iphonesimulator arm64-apple-ios15.0-simulator ios-sim-arm64.swiftmodule" \
    "iphonesimulator x86_64-apple-ios15.0-simulator ios-sim-x86_64.swiftmodule" \
    "watchos arm64_32-apple-watchos10.0 watch-arm64_32.swiftmodule" \
    "watchos arm64-apple-watchos26.0 watch-arm64.swiftmodule" \
    "watchsimulator arm64-apple-watchos10.0-simulator watch-sim-arm64.swiftmodule" \
    "watchsimulator x86_64-apple-watchos10.0-simulator watch-sim-x86_64.swiftmodule"
do
    fields=(${=platform})
    sdk=${fields[1]}
    target=${fields[2]}
    output=${fields[3]}
    sdk_path=$(xcrun --sdk "$sdk" --show-sdk-path)
    slice_directory="$temporary_directory/$output"
    mkdir -p "$slice_directory"
    xcrun swiftc \
        "${common_flags[@]}" \
        -sdk "$sdk_path" \
        -target "$target" \
        -emit-module \
        -module-name iSHAppleSwift \
        -emit-module-path "$slice_directory/iSHAppleSwift.swiftmodule" \
        "${sources[@]}"
    xcrun swiftc \
        "${common_flags[@]}" \
        -sdk "$sdk_path" \
        -target "$target" \
        -I "$slice_directory" \
        -typecheck \
        "$compile_test"
    xcrun swiftc \
        "${common_flags[@]}" \
        -sdk "$sdk_path" \
        -target "$target" \
        -I "$slice_directory" \
        -typecheck \
        "$consumer"
done

print "Swift 6 公共包装与独立消费者 typecheck 通过（含 arm64_32）"
