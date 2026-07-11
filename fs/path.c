#include <string.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "fs/path.h"

static char *path_root_floor(char *out, char *current,
        const char *root_path) {
    if (strcmp(root_path, "/") == 0)
        return out;
    size_t root_length = strlen(root_path);
    size_t current_length = (size_t) (current - out);
    if (current_length >= root_length &&
            memcmp(out, root_path, root_length) == 0 &&
            (current_length == root_length || out[root_length] == '/'))
        return out + root_length;
    return out;
}

static int __path_normalize(struct task *task,
        const char *at_path, const char *path, char *out,
        int flags, int levels, const char *root_path) {
    // you must choose one
    if (flags & N_SYMLINK_FOLLOW)
        assert(!(flags & N_SYMLINK_NOFOLLOW));
    else
        assert(flags & N_SYMLINK_NOFOLLOW);

    const char *p = path;
    char *o = out;
    *o = '\0';
    int n = MAX_PATH - 1;

    if (strcmp(path, "") == 0)
        return _ENOENT;

    if (at_path != NULL && strcmp(at_path, "/") != 0) {
        strcpy(o, at_path);
        n -= strlen(at_path);
        o += strlen(at_path);
    }

    while (*p == '/')
        p++;

    while (*p != '\0') {
        if (p[0] == '.') {
            if (p[1] == '\0' || p[1] == '/') {
                // single dot path component, ignore
                p++;
                while (*p == '/')
                    p++;
                continue;
            } else if (p[1] == '.' && (p[2] == '\0' || p[2] == '/')) {
                // double dot path component, delete the last component
                char *floor = path_root_floor(out, o, root_path);
                if (o > floor) {
                    do {
                        o--;
                        n++;
                    } while (o > floor && *o != '/');
                }
                p += 2;
                while (*p == '/')
                    p++;
                continue;
            }
        }

        // output a slash
        *o++ = '/'; n--;
        char *c = o;
        // copy up to a slash or null
        while (*p != '/' && *p != '\0' && --n > 0)
            *o++ = *p++;
        bool followed_by_slash = *p == '/';
        // eat any slashes
        while (*p == '/')
            p++;

        if (n == 0)
            return _ENAMETOOLONG;

        if ((flags & N_SYMLINK_FOLLOW) || *p != '\0' || followed_by_slash) {
            // this buffer is used to store the path that we're readlinking, then
            // if it turns out to point to a symlink it's reused as the buffer
            // passed to the next path_normalize call
            char possible_symlink[MAX_PATH];
            *o = '\0';
            strcpy(possible_symlink, out);
            struct mount *mount = find_mount_and_trim_path(possible_symlink);
            assert(path_is_normalized(possible_symlink));
            int res = _EINVAL;
            if (mount->fs->readlink)
                res = mount->fs->readlink(mount, possible_symlink, c, MAX_PATH - (c - out));
            if (res >= 0) {
                mount_release(mount);
                if (levels >= 5)
                    return _ELOOP;
                // readlink does not null terminate
                c[res] = '\0';
                const char *restart_at = NULL;
                // if we should restart from the root, copy down
                if (*c == '/') {
                    memmove(out, c, strlen(c) + 1);
                    restart_at = root_path;
                }
                char *expanded_path = possible_symlink;
                strcpy(expanded_path, out);
                if (strcmp(p, "") != 0) {
                    strcat(expanded_path, "/");
                    strcat(expanded_path, p);
                } else if (followed_by_slash) {
                    if (strlen(expanded_path) + 1 >= MAX_PATH)
                        return _ENAMETOOLONG;
                    strcat(expanded_path, "/");
                }
                return __path_normalize(task, restart_at, expanded_path,
                        out, flags, levels + 1, root_path);
            }

            // if there's a slash after this component, ensure that if it
            // exists, it's a directory and that we have execute perms on it
            if (followed_by_slash) {
                struct statbuf stat = {};
                int err = mount->fs->stat(mount, possible_symlink, &stat);
                mount_release(mount);
                if (err >= 0) {
                    if (!S_ISDIR(stat.mode))
                        return _ENOTDIR;
                    err = access_check_task(task, &stat, AC_X);
                    if (err < 0)
                        return err;
                }
            } else {
                mount_release(mount);
            }
        }
    }

    *o = '\0';
    assert(path_is_normalized(out));

    return 0;
}

int path_normalize_task(struct task *task, struct fd *at,
        const char *path, char *out, int flags) {
    assert(at != NULL);
    if (strcmp(path, "") == 0)
        return _ENOENT;

    // start with root or cwd, depending on whether it starts with a slash
    char at_path[MAX_PATH + 1];
    char root_path[MAX_PATH + 1];
    struct fs_info *fs = task->fs;
    lock(&fs->lock);
    int root_error = generic_getpath(fs->root, root_path);
    if (root_error < 0) {
        unlock(&fs->lock);
        return root_error;
    }
    assert(path_is_normalized(root_path));
    bool fs_path = path[0] == '/' || at == AT_PWD;
    if (fs_path) {
        if (path[0] == '/') {
            at = fs->root;
            strcpy(at_path, root_path);
        } else {
            at = fs->pwd;
        }
        int err = path[0] == '/' ? 0 : generic_getpath(at, at_path);
        unlock(&fs->lock);
        if (err < 0)
            return err;
        assert(path_is_normalized(at_path));
    } else if (at != NULL) {
        unlock(&fs->lock);
        int err = generic_getpath(at, at_path);
        if (err < 0)
            return err;
        assert(path_is_normalized(at_path));
    } else {
        unlock(&fs->lock);
    }

    return __path_normalize(task, at != NULL ? at_path : NULL,
            path, out, flags, 0, root_path);
}

int path_normalize(struct fd *at, const char *path, char *out, int flags) {
    return path_normalize_task(current, at, path, out, flags);
}


bool path_is_normalized(const char *path) {
    while (*path != '\0') {
        if (*path != '/')
            return false;
        path++;
        if (*path == '/')
            return false;
        while (*path != '/' && *path != '\0')
            path++;
    }
    return true;
}

bool path_next_component(const char **path, char *component, int *err) {
    const char *p = *path;
    if (*p == '\0')
        return false;

    assert(*p == '/');
    p++;
    char *c = component;
    while (*p != '/' && *p != '\0') {
        *c++ = *p++;
        if (c - component >= MAX_NAME) {
            *err = _ENAMETOOLONG;
            return false;
        }
    }
    *c = '\0';
    *path = p;
    return true;
}
