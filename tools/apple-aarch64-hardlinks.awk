BEGIN {
    OFS = "\t"
}

{
    # 加前缀强制按字符串比较，避免大 inode 被浮点数舍入后误合并。
    inode_key = "inode:" $1
    if (inode_key != previous_inode_key) {
        canonical = $2
        previous_inode_key = inode_key
    }
    print canonical, $2
}
