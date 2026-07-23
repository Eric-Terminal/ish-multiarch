#!/bin/bash
set -euo pipefail

resources="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH"
seed="$resources/AArch64Rootfs.seed"
owner="$DERIVED_FILE_DIR/AArch64Rootfs.seed.owner"
seed_complete=1
for output in "$seed/data/bin/busybox" "$seed/meta.db" \
        "$seed/rootfs-hardlinks.tsv" "$seed/rootfs-manifest.txt"; do
    [[ -f "$output" ]] || seed_complete=0
done

# 只清理由本阶段登记的残缺产物，未知目录继续交给安全打包器拒绝。
if (( !seed_complete )) && [[ -f "$owner" && ! -L "$owner" ]] && \
        [[ $(sed -n '1p' "$owner") == "$seed" ]]; then
    rm -rf "$seed"
fi
rmdir "$seed/data/bin" "$seed/data" "$seed" 2>/dev/null || true
"$SRCROOT/tools/apple-aarch64-rootfs.sh" "$seed"
rm -f "$resources/root.tar.gz"
touch -m -r "$seed/rootfs-manifest.txt" "$seed/data/bin/busybox"

owner_temporary="$owner.tmp.$$"
trap 'rm -f "$owner_temporary"' EXIT
printf '%s\n' "$seed" > "$owner_temporary"
mv -f "$owner_temporary" "$owner"
trap - EXIT
