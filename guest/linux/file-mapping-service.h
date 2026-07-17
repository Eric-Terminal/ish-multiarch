#ifndef GUEST_LINUX_FILE_MAPPING_SERVICE_H
#define GUEST_LINUX_FILE_MAPPING_SERVICE_H

#include "guest/memory/file-pager.h"

struct guest_linux_file_mapping_request {
    qword_t fd;
    qword_t offset;
    qword_t length;
    qword_t protection;
    qword_t flags;
};

struct guest_linux_file_mapping_context {
    void *runtime_opaque;
    void *task_opaque;
};

// fd 句柄只在 mapping service 内解释；runtime 负责成对 acquire/release。
struct guest_linux_file_mapping_handle {
    void *opaque;
};

struct guest_linux_file_mapping {
    struct guest_file_pager *pager;
    qword_t maximum_protection;
};

/* acquire 只固定 fd 身份，使 EBADF 先于 do_mmap 的通用参数检查。 */
typedef sdword_t (*guest_linux_file_mapping_acquire)(
        const struct guest_linux_file_mapping_context *context,
        qword_t fd, struct guest_linux_file_mapping_handle *handle);
typedef void (*guest_linux_file_mapping_release)(
        struct guest_linux_file_mapping_handle *handle);
/* open 在地址预检成功后执行；成功交出 pager 强引用且不消费 handle。 */
typedef sdword_t (*guest_linux_file_mapping_open)(
        const struct guest_linux_file_mapping_handle *handle,
        const struct guest_linux_file_mapping_request *request,
        struct guest_linux_file_mapping *mapping);

struct guest_linux_file_mapping_service {
    void *runtime_opaque;
    guest_linux_file_mapping_acquire acquire;
    guest_linux_file_mapping_release release;
    guest_linux_file_mapping_open open;
};

_Static_assert(sizeof(struct guest_linux_file_mapping_request) == 40 &&
        offsetof(struct guest_linux_file_mapping_request, fd) == 0 &&
        offsetof(struct guest_linux_file_mapping_request, offset) == 8 &&
        offsetof(struct guest_linux_file_mapping_request, length) == 16 &&
        offsetof(struct guest_linux_file_mapping_request, protection) == 24 &&
        offsetof(struct guest_linux_file_mapping_request, flags) == 32,
        "文件映射请求必须保持 host ABI 无关的五个 64 位字段");

#endif
