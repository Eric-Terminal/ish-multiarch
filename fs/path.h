#ifndef PATH_H
#define PATH_H

struct task;
struct fs_access_identity;

#define AT_PWD (struct fd *) -2

#define N_SYMLINK_FOLLOW 1
#define N_SYMLINK_NOFOLLOW 2
#define N_PARENT_DIR_WRITE 4

// Normalizes the path specified and writes the result into the out buffer.
//
// Normalization means:
//  - prepending the current or root directory
//  - converting multiple slashes into one
//  - resolving . and ..
//  - resolving symlinks, skipping the last path component if the follow_links
//    argument is true
// The result will always begin with a slash or be empty.
//
// If the normalized path plus the null terminator would be longer than
// MAX_PATH, _ENAMETOOLONG is returned. The out buffer is expected to be at
// least MAX_PATH in size.
//
// at 是相对路径的解析基准。AT_PWD 在 task 版中使用 task->fs->pwd，
// 兼容入口则使用 current->fs->pwd，并在读取路径期间保持对应 fs 锁。
int path_normalize_task(struct task *task, struct fd *at,
        const char *path, char *out, int flags);
int path_normalize_task_access(struct task *task, struct fd *at,
        const char *path, char *out, int flags,
        const struct fs_access_identity *identity);
int path_normalize(struct fd *at, const char *path, char *out, int flags);
bool path_is_normalized(const char *path);

// Helper function for iterating through a normalized path.
//
// The *path pointer is advanced to point to the next /, and the next path
// component is copied to component. component must point to a buffer large
// enough to hold a string of MAX_NAME characters.
//
// If the next path component was successfully copied, returns true; otherwise
// returns false. If an error occurred, *err is set to the error code.
// Otherwise, the end of the path has been reached.
bool path_next_component(const char **path, char *component, int *err);

#endif
