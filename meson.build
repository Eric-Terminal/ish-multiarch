project('ish', 'c',
    default_options: ['default_library=static', 'c_std=gnu11', 'warning_level=2'])
cc = meson.get_compiler('c')

if cc.get_id() == 'clang'
    add_project_arguments('-Wimplicit-fallthrough', '-Wtautological-constant-in-range-compare', language: 'c')
endif

if get_option('b_sanitize').split(',').contains('undefined')
    add_project_arguments('-fno-sanitize=alignment', language: 'c')
endif

log_on = get_option('log').split()
log_off = get_option('nolog').split()
foreach channel : log_on + log_off
    if log_on.contains(channel)
        add_project_arguments('-DDEBUG_' + channel + '=1', language: 'c')
    else
        add_project_arguments('-DDEBUG_' + channel + '=0', language: 'c')
    endif
endforeach
add_project_arguments('-DLOG_HANDLER_' + get_option('log_handler').to_upper() + '=1', language: 'c')
add_project_arguments('-DENGINE_' + get_option('engine').to_upper() + '=1', language: 'c')

if get_option('no_crlf')
    add_project_arguments('-DNO_CRLF', language: 'c')
endif

add_project_arguments('-Wno-switch', language: 'c')

includes = [include_directories('.')]

aarch64_backend_option = get_option('aarch64_backend')
aarch64_host_supports_threaded = host_machine.cpu_family() == 'aarch64'
if aarch64_backend_option == 'threaded' and not aarch64_host_supports_threaded
    error('threaded AArch64 后端只能用于 aarch64 主机')
endif
aarch64_threaded_default = aarch64_backend_option == 'threaded' or \
        (aarch64_backend_option == 'auto' and aarch64_host_supports_threaded)
aarch64_backend_config = configuration_data()
aarch64_backend_config.set10(
        'ISH_AARCH64_BACKEND_THREADED_DEFAULT', aarch64_threaded_default)
configure_file(output: 'aarch64-backend-config.h',
    configuration: aarch64_backend_config)

aarch64_backend_src = [
    'guest/aarch64/backend.c',
    'guest/aarch64/runner.c',
    'guest/aarch64/threaded.c',
]

aarch64_core_src = [
    'guest/memory/address-space.c',
    'guest/memory/tlb.c',
    'guest/memory/page-backing.c',
    'guest/memory/page-table.c',
    'guest/linux/user-memory.c',
    'guest/aarch64/decode.c',
    'guest/aarch64/condition.c',
    'guest/aarch64/execute.c',
] + aarch64_backend_src + [
    'guest/aarch64/linux-syscall.c',
    'guest/aarch64/linux-signal-info.c',
    'guest/aarch64/linux-signal-frame.c',
    'guest/aarch64/linux-signal-trampoline.c',
    'guest/aarch64/linux-process.c',
    'guest/linux/vma.c',
    'guest/linux/memory.c',
    'guest/aarch64/linux-runtime.c',
    'guest/aarch64/elf64.c',
    'guest/aarch64/elf64-loader.c',
    'guest/aarch64/linux-stack.c',
]
libish_aarch64_core = static_library('ish_aarch64_core', aarch64_core_src,
    include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
aarch64_core_link_smoke = executable('aarch64_core_link_smoke',
    'guest/aarch64/core-link-smoke.c',
    include_directories: includes,
    c_args: ['-DISH_GUEST_AARCH64=1'],
    link_whole: libish_aarch64_core)
if not meson.is_cross_build()
    test('aarch64_core_link', aarch64_core_link_smoke)
endif

if host_machine.system() == 'darwin'
    darwin_platform_link_smoke = executable('darwin_platform_link_smoke', [
            'platform/darwin.c',
            'platform/darwin-link-smoke.c',
        ], include_directories: includes)
    if not meson.is_cross_build()
        test('darwin_platform_link', darwin_platform_link_smoke)
    endif
endif

if not get_option('core_only')
threads = dependency('threads')
librt = cc.find_library('rt', required: false)
libm = cc.find_library('m', required: false)
libdl = cc.find_library('dl', required: false)
sqlite3 = cc.find_library('sqlite3')
dependencies = [librt, libm, libdl, threads]

subdir('vdso') # ish depends on the vdso

offsets = custom_target('offsets',
    output: 'cpu-offsets.h', input: 'asbestos/offsets.c', depfile: 'cpu-offsets.h.d',
    command: [find_program('tools/staticdefine.sh'), '@OUTDIR@/compile_commands.json', '@INPUT@', '@OUTPUT@', '@DEPFILE@'])

emu_src = [
    'emu/tlb.c',
    'emu/fpu.c',
    'emu/vec.c',
    'emu/mmx.c',
    'emu/float80.c',

]
gadgets = 'asbestos/gadgets-' + host_machine.cpu_family()
emu_src += [
    'asbestos/asbestos.c',
    'asbestos/gen.c',
    'asbestos/helpers.c',
    gadgets+'/entry.S',
    gadgets+'/memory.S',
    gadgets+'/control.S',
    gadgets+'/math.S',
    gadgets+'/bits.S',
    gadgets+'/string.S',
    gadgets+'/misc.S',
    offsets,
]

libish_emu = library('ish_emu', emu_src, include_directories: includes)

libfakefs = library('fakefs',
    ['fs/fake-db.c', 'fs/fake-migrate.c', 'fs/fake-rebuild.c'],
    include_directories: includes,
    dependencies: sqlite3)

subdir('deps')

if get_option('kernel') == 'ish'
    if get_option('engine') != 'asbestos'
        error('Only asbestos is supported with ish kernel')
    endif

    src = [
        'kernel/init.c',
        'kernel/errno.c',

        'kernel/calls.c',
        'kernel/memory.c',
        'kernel/user.c',
        'kernel/vdso.c', vdso,
        'kernel/task.c',
        'kernel/group.c',
        'kernel/log.c',

        'kernel/fork.c',
        'kernel/exec.c',
        'kernel/exit.c',
        'kernel/time.c',
        'kernel/mmap.c',
        'kernel/uname.c',
        'kernel/tls.c',
        'kernel/futex.c',
        'kernel/getset.c',
        'kernel/signal.c',
        'kernel/signal-queue.c',
        'kernel/signal-delivery.c',
        'kernel/signal-wire.c',
        'kernel/signal-altstack.c',
        'kernel/resource.c',
        'kernel/random.c',
        'kernel/misc.c',
        'kernel/eventfd.c',
        'kernel/ipc.c',
        'kernel/ptrace.c',

        'kernel/fs.c',
        'kernel/fs-task.c',
        'kernel/aarch64-exec.c',
        'kernel/aarch64-exec-image.c',
        'kernel/aarch64-fd-service.c',
        'kernel/aarch64-resource-wire.c',
        'kernel/aarch64-task-runner.c',
        'kernel/aarch64-signal-service.c',
        'kernel/aarch64-syscall-service.c',
        'kernel/aarch64-time-service.c',
        'kernel/aarch64-wait-service.c',
        'kernel/fs_info.c',
        'fs/mount.c',
        'fs/fd.c',
        'fs/inode.c',
        'fs/stat.c',
        'fs/dir.c',
        'fs/generic.c',
        'fs/path.c',
        'fs/real.c',
        'fs/fake.c',

        'fs/proc.c',
        'fs/proc/entry.c',
        'fs/proc/ish.c',
        'fs/proc/root.c',
        'fs/proc/pid.c',

        'fs/dyndev.c',

        'fs/adhoc.c',
        'fs/sock.c',
        'fs/pipe.c',
        'fs/sockrestart.c',
        'fs/lock.c',

        'fs/dev.c',
        'fs/mem.c',
        'fs/tty.c',
        'fs/tty-real.c',
        'fs/pty.c',
        'fs/tmp.c',

        'fs/poll.c',
        'kernel/poll.c',
        'kernel/epoll.c',

        'util/timer.c',
        'util/sync.c',
        'util/fifo.c',
        'util/fchdir.c',

        'platform/' + host_machine.system() + '.c',
    ]

    libish = library('ish', src,
        include_directories: includes,
        link_whole: libish_aarch64_core)
    ish = declare_dependency(
        link_with: [libish, libish_emu, libfakefs],
        dependencies: dependencies,
        include_directories: includes)

    if host_machine.system() == 'darwin'
        apple_runtime_link_smoke = executable(
            'apple_runtime_link_smoke',
            'platform/apple-runtime-link-smoke.c',
            dependencies: ish,
            link_args: ['-Wl,-fatal_warnings'])
        apple_runtime_force_link_smoke = executable(
            'apple_runtime_force_link_smoke',
            'platform/apple-runtime-link-smoke.c',
            link_whole: [libish, libish_emu, libfakefs],
            dependencies: dependencies,
            link_args: ['-Wl,-fatal_warnings'])
    endif

    if not meson.is_cross_build()
        executable('ish', ['main.c'], dependencies: ish)

        fd_task_test = executable('fd_task_test', 'fs/fd-task-test.c', dependencies: ish)
        test('fd_task', fd_task_test)

        file_lock_test = executable(
            'file_lock_test', 'fs/lock-test.c', dependencies: ish)
        test('file_lock', file_lock_test)

        proc_pid_test = executable(
            'proc_pid_test', 'fs/proc-pid-test.c', dependencies: ish)
        test('proc_pid', proc_pid_test)

        poll_oneshot_test = executable(
            'poll_oneshot_test', 'fs/poll-oneshot-test.c', dependencies: ish)
        test('poll_oneshot', poll_oneshot_test)

        select_lifecycle_test = executable(
            'select_lifecycle_test',
            'kernel/select-lifecycle-test.c',
            dependencies: ish)
        test('select_lifecycle', select_lifecycle_test)

        sigmask_wait_test = executable(
            'sigmask_wait_test',
            'kernel/sigmask-wait-test.c',
            dependencies: ish)
        test('sigmask_wait', sigmask_wait_test)

        fs_task_test = executable('fs_task_test', 'kernel/fs-task-test.c', dependencies: ish)
        test('fs_task', fs_task_test)

        dir_task_test = executable('dir_task_test', 'fs/dir-task-test.c', dependencies: ish)
        test('dir_task', dir_task_test)

        stat_convert_test = executable(
            'stat_convert_test', 'fs/stat-convert-test.c', dependencies: ish)
        test('stat_convert', stat_convert_test)

        path_task_test = executable('path_task_test', 'fs/path-task-test.c', dependencies: ish)
        test('path_task', path_task_test)

        aarch64_syscall_service_test = executable(
            'aarch64_syscall_service_test',
            'kernel/aarch64-syscall-service-test.c',
            dependencies: ish)
        test('aarch64_syscall_service', aarch64_syscall_service_test)

        aarch64_file_syscall_test = executable(
            'aarch64_file_syscall_test',
            'kernel/aarch64-file-syscall-test.c',
            dependencies: ish)
        test('aarch64_file_syscall', aarch64_file_syscall_test)

        aarch64_lseek_syscall_test = executable(
            'aarch64_lseek_syscall_test',
            'kernel/aarch64-lseek-syscall-test.c',
            dependencies: ish)
        test('aarch64_lseek_syscall', aarch64_lseek_syscall_test)

        aarch64_fd_syscall_test = executable(
            'aarch64_fd_syscall_test',
            'kernel/aarch64-fd-syscall-test.c',
            dependencies: ish)
        test('aarch64_fd_syscall', aarch64_fd_syscall_test)

        aarch64_process_syscall_test = executable(
            'aarch64_process_syscall_test',
            'kernel/aarch64-process-syscall-test.c',
            dependencies: ish)
        test('aarch64_process_syscall', aarch64_process_syscall_test)

        aarch64_sigsuspend_syscall_test = executable(
            'aarch64_sigsuspend_syscall_test',
            'kernel/aarch64-sigsuspend-syscall-test.c',
            dependencies: ish)
        test('aarch64_sigsuspend_syscall',
            aarch64_sigsuspend_syscall_test)

        aarch64_nanosleep_syscall_test = executable(
            'aarch64_nanosleep_syscall_test',
            'kernel/aarch64-nanosleep-syscall-test.c',
            dependencies: ish)
        test('aarch64_nanosleep_syscall',
            aarch64_nanosleep_syscall_test)

        aarch64_clock_gettime_syscall_test = executable(
            'aarch64_clock_gettime_syscall_test',
            'kernel/aarch64-clock-gettime-syscall-test.c',
            dependencies: ish)
        test('aarch64_clock_gettime_syscall',
            aarch64_clock_gettime_syscall_test)

        aarch64_clone_test = executable(
            'aarch64_clone_test',
            'kernel/aarch64-clone-test.c',
            dependencies: ish)
        test('aarch64_clone', aarch64_clone_test)

        aarch64_futex_test = executable(
            'aarch64_futex_test',
            'kernel/aarch64-futex-test.c',
            dependencies: ish)
        test('aarch64_futex', aarch64_futex_test)

        aarch64_clone3_syscall_test = executable(
            'aarch64_clone3_syscall_test',
            'kernel/aarch64-clone3-syscall-test.c',
            dependencies: ish)
        test('aarch64_clone3_syscall',
            aarch64_clone3_syscall_test)

        aarch64_exec_image_test = executable(
            'aarch64_exec_image_test',
            'kernel/aarch64-exec-image-test.c',
            dependencies: ish)
        test('aarch64_exec_image', aarch64_exec_image_test)

        aarch64_sigaltstack_test = executable(
            'aarch64_sigaltstack_test',
            'kernel/aarch64-sigaltstack-test.c',
            dependencies: ish)
        test('aarch64_sigaltstack', aarch64_sigaltstack_test)

        aarch64_signal_service_test = executable(
            'aarch64_signal_service_test',
            'kernel/aarch64-signal-service-test.c',
            dependencies: ish)
        test('aarch64_signal_service', aarch64_signal_service_test)

        signal_action_test = executable(
            'signal_action_test',
            'kernel/signal-action-test.c',
            dependencies: ish)
        test('signal_action', signal_action_test)

        signal_wire_i386_test = executable('signal_wire_i386_test', [
                'kernel/signal-wire.c',
                'kernel/signal-wire-test.c',
            ], include_directories: includes,
            c_args: ['-DISH_GUEST_I386=1'])
        test('signal_wire_i386', signal_wire_i386_test)

        signal_wire_aarch64_test = executable('signal_wire_aarch64_test', [
                'kernel/signal-wire.c',
                'kernel/signal-wire-test.c',
            ], include_directories: includes,
            c_args: ['-DISH_GUEST_AARCH64=1'])
        test('signal_wire_aarch64', signal_wire_aarch64_test)

        linux_signal_service_i386_probe = static_library(
            'linux_signal_service_i386_probe',
            'guest/linux/signal-service-backend-test.c',
            include_directories: includes,
            c_args: ['-DISH_GUEST_I386=1'])
        linux_signal_service_bridge_test = executable(
            'linux_signal_service_bridge_test',
            'guest/linux/signal-service-bridge-test.c',
            include_directories: includes,
            c_args: ['-DISH_GUEST_AARCH64=1'],
            link_with: linux_signal_service_i386_probe)
        test('linux_signal_service_bridge',
            linux_signal_service_bridge_test)

        signal_lock_test = executable(
            'signal_lock_test',
            'kernel/signal-lock-test.c',
            dependencies: ish)
        test('signal_lock', signal_lock_test)

        signal_queue_test = executable(
            'signal_queue_test',
            'kernel/signal-queue-test.c',
            dependencies: ish)
        test('signal_queue', signal_queue_test)

        child_exit_policy_test = executable(
            'child_exit_policy_test',
            'kernel/child-exit-policy-test.c',
            dependencies: ish)
        test('child_exit_policy', child_exit_policy_test)

        user_test = executable(
            'user_test',
            'kernel/user-test.c',
            dependencies: ish)
        test('user', user_test)

        timer_lifecycle_test = executable(
            'timer_lifecycle_test',
            'util/timer-lifecycle-test.c',
            dependencies: ish)
        test('timer_lifecycle', timer_lifecycle_test)

        timer_syscall_test = executable(
            'timer_syscall_test',
            'kernel/timer-syscall-test.c',
            dependencies: ish)
        test('timer_syscall', timer_syscall_test)

        clone_failure_test = executable(
            'clone_failure_test',
            'kernel/clone-failure-test.c',
            dependencies: ish)
        test('clone_failure', clone_failure_test)

        task_publication_test = executable(
            'task_publication_test',
            'kernel/task-publication-test.c',
            dependencies: ish)
        test('task_publication', task_publication_test)

        aarch64_task_lifecycle_test = executable(
            'aarch64_task_lifecycle_test',
            'kernel/aarch64-task-lifecycle-test.c',
            dependencies: ish)
        test('aarch64_task_lifecycle',
            aarch64_task_lifecycle_test)

        aarch64_task_runner_test = executable(
            'aarch64_task_runner_test',
            'kernel/aarch64-task-runner-test.c',
            dependencies: ish)
        test('aarch64_task_runner', aarch64_task_runner_test)
    endif

elif get_option('kernel') == 'linux'
    kernel_src = [
        'linux/main.c',
        'linux/fakefs.c',
    ]
    user_src = []
    emu_deps = []

    if get_option('engine') == 'asbestos'
        user_src += 'linux/emu_asbestos.c'
        emu_deps += declare_dependency(link_with: libish_emu)
    elif get_option('engine') == 'unicorn'
        user_src += 'linux/emu_unicorn.c'
        kernel_src += 'linux/emu_unicorn_kernel.c'
        emu_deps += declare_dependency(
            dependencies: [cc.find_library('unicorn', dirs: [meson.current_source_dir()+'/deps/unicorn/build'])],
            include_directories: include_directories('deps/unicorn/include'),
        )
    endif

    modules = static_library('linux_modules', kernel_src, dependencies: [linux_headers, sqlite3])
    user_modules = static_library('linux_user', user_src, dependencies: [user_linux_headers] + emu_deps)
    executable('ish',
        build_linux,
        link_with: [libfakefs],
        dependencies: [
            liblinux,
            libm,
            libdl,
            threads,
            declare_dependency(link_whole: [modules, user_modules]),
        ] + emu_deps)
endif

subdir('tools')

gdb_scripts = ['ish-gdb.gdb']
foreach script : gdb_scripts
    custom_target(script,
        output: script, input: script,
        command: ['ln', '-sf', '@INPUT@', '@OUTPUT@'],
        build_by_default: true)
endforeach

if not meson.is_cross_build()
    apple_meson_arch_test = find_program('tools/apple-meson-arch-test.sh')
    test('apple_meson_arch', apple_meson_arch_test)

    # test for floating point library
    float80_test = executable('float80_test',
        ['emu/float80.c', 'emu/fpu.c', 'emu/float80-test.c'], dependencies: [libm])
    test('float80', float80_test)

    fiber_cell_test = executable('fiber_cell_test', 'asbestos/cell-test.c', include_directories: includes)
    test('fiber_cell', fiber_cell_test)

    i386_cpu_test = executable('i386_cpu_test', 'guest/i386/cpu-test.c', include_directories: includes)
    test('i386_cpu', i386_cpu_test)

    i386_tlb_test = executable('i386_tlb_test', ['emu/tlb.c', 'emu/tlb-test.c'],
        include_directories: includes)
    test('i386_tlb', i386_tlb_test)

    i386_types_test = executable('i386_types_test', 'guest/target-types-test.c', include_directories: includes)
    test('i386_types', i386_types_test)

    aarch64_types_test = executable('aarch64_types_test', 'guest/target-types-test.c',
        include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_types', aarch64_types_test)

    linux_syscall_service_i386_probe = static_library(
        'linux_syscall_service_i386_probe',
        'guest/linux/syscall-service-backend-test.c',
        include_directories: includes, c_args: ['-DISH_GUEST_I386=1'])
    linux_syscall_service_bridge_test = executable(
        'linux_syscall_service_bridge_test',
        'guest/linux/syscall-service-bridge-test.c',
        include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1'],
        link_with: linux_syscall_service_i386_probe)
    test('linux_syscall_service_bridge', linux_syscall_service_bridge_test)

    aarch64_cpu_test = executable('aarch64_cpu_test', 'guest/aarch64/cpu-test.c',
        include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_cpu', aarch64_cpu_test)

    aarch64_condition_test = executable('aarch64_condition_test', [
            'guest/aarch64/condition.c',
            'guest/aarch64/condition-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_condition', aarch64_condition_test)

    aarch64_conditional_compare_test = executable(
        'aarch64_conditional_compare_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/conditional-compare-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_conditional_compare', aarch64_conditional_compare_test)

    aarch64_linux_syscall_test = executable('aarch64_linux_syscall_test', [
            'guest/aarch64/linux-syscall.c',
            'guest/aarch64/linux-syscall-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_linux_syscall', aarch64_linux_syscall_test)

    aarch64_linux_file_abi_test = executable('aarch64_linux_file_abi_test',
        'guest/aarch64/linux-file-abi-test.c',
        include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_linux_file_abi', aarch64_linux_file_abi_test)

    aarch64_linux_signal_abi_test = executable(
        'aarch64_linux_signal_abi_test',
        'guest/aarch64/linux-signal-abi-test.c',
        include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_linux_signal_abi', aarch64_linux_signal_abi_test)

    aarch64_linux_signal_info_test = executable(
        'aarch64_linux_signal_info_test',
        'guest/aarch64/linux-signal-info-test.c',
        include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1'],
        link_with: libish_aarch64_core)
    test('aarch64_linux_signal_info', aarch64_linux_signal_info_test)

    aarch64_linux_signal_frame_test = executable(
        'aarch64_linux_signal_frame_test',
        'guest/aarch64/linux-signal-frame-test.c',
        include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1'],
        link_with: libish_aarch64_core)
    test('aarch64_linux_signal_frame', aarch64_linux_signal_frame_test)

    aarch64_linux_runtime_test = executable('aarch64_linux_runtime_test',
        'guest/aarch64/linux-runtime-test.c',
        include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1'],
        link_with: libish_aarch64_core)
    test('aarch64_linux_runtime', aarch64_linux_runtime_test)

    aarch64_linux_runtime_signal_test = executable(
        'aarch64_linux_runtime_signal_test',
        'guest/aarch64/linux-runtime-signal-test.c',
        include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1'],
        link_with: libish_aarch64_core)
    test('aarch64_linux_runtime_signal',
        aarch64_linux_runtime_signal_test)

    aarch64_linux_rt_sigreturn_test = executable(
        'aarch64_linux_rt_sigreturn_test',
        'guest/aarch64/linux-rt-sigreturn-test.c',
        include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1'],
        link_with: libish_aarch64_core)
    test('aarch64_linux_rt_sigreturn',
        aarch64_linux_rt_sigreturn_test)

    aarch64_linux_signal_trampoline_test = executable(
        'aarch64_linux_signal_trampoline_test',
        'guest/aarch64/linux-signal-trampoline-test.c',
        include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1'],
        link_with: libish_aarch64_core)
    test('aarch64_linux_signal_trampoline',
        aarch64_linux_signal_trampoline_test)

    aarch64_linux_process_test = executable(
        'aarch64_linux_process_test',
        'guest/aarch64/linux-process-test.c',
        include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1'],
        link_with: libish_aarch64_core)
    test('aarch64_linux_process', aarch64_linux_process_test)

    aarch64_linux_process_fork_test = executable(
        'aarch64_linux_process_fork_test',
        aarch64_core_src + ['guest/aarch64/linux-process-fork-test.c'],
        include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1',
            '-DGUEST_PAGE_TABLE_TESTING=1',
            '-DAARCH64_LINUX_PROCESS_TESTING=1'])
    test('aarch64_linux_process_fork',
        aarch64_linux_process_fork_test)

    aarch64_linux_dynamic_process_test = executable(
        'aarch64_linux_dynamic_process_test',
        'guest/aarch64/linux-dynamic-process-test.c',
        include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1'],
        link_with: libish_aarch64_core)
    test('aarch64_linux_dynamic_process',
        aarch64_linux_dynamic_process_test)

    aarch64_linux_user_memory_test = executable(
        'aarch64_linux_user_memory_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/memory/page-backing.c',
            'guest/memory/page-table.c',
            'guest/linux/user-memory.c',
            'guest/linux/user-memory-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_linux_user_memory', aarch64_linux_user_memory_test)

    guest_linux_memory_test = executable('guest_linux_memory_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/memory/page-backing.c',
            'guest/memory/page-table.c',
            'guest/linux/vma.c',
            'guest/linux/memory.c',
            'guest/linux/memory-test.c',
        ], include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1',
            '-DGUEST_LINUX_VMA_TESTING=1',
            '-DGUEST_PAGE_BACKING_TESTING=1'])
    test('guest_linux_memory', guest_linux_memory_test)

    guest_linux_memory_shared_test = executable(
        'guest_linux_memory_shared_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/memory/page-backing.c',
            'guest/memory/page-table.c',
            'guest/linux/vma.c',
            'guest/linux/memory.c',
            'guest/linux/memory-shared-test.c',
        ], include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1',
            '-DGUEST_LINUX_VMA_TESTING=1',
            '-DGUEST_PAGE_BACKING_TESTING=1',
            '-DGUEST_PAGE_TABLE_TESTING=1'],
        dependencies: threads)
    test('guest_linux_memory_shared', guest_linux_memory_shared_test)

    guest_linux_vma_test = executable('guest_linux_vma_test', [
            'guest/linux/vma.c',
            'guest/linux/vma-test.c',
        ], include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1',
            '-DGUEST_LINUX_VMA_TESTING=1'])
    test('guest_linux_vma', guest_linux_vma_test)

    aarch64_elf64_test = executable('aarch64_elf64_test', [
            'guest/aarch64/elf64.c',
            'guest/aarch64/elf64-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_elf64', aarch64_elf64_test)

    aarch64_elf64_loader_test = executable('aarch64_elf64_loader_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/memory/page-backing.c',
            'guest/memory/page-table.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
        ] + aarch64_backend_src + [
            'guest/aarch64/linux-syscall.c',
            'guest/aarch64/elf64.c',
            'guest/aarch64/elf64-loader.c',
            'guest/aarch64/elf64-loader-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_elf64_loader', aarch64_elf64_loader_test)

    aarch64_linux_stack_test = executable('aarch64_linux_stack_test', [
            'guest/memory/address-space.c',
            'guest/memory/page-backing.c',
            'guest/memory/page-table.c',
            'guest/memory/tlb.c',
            'guest/aarch64/linux-stack.c',
            'guest/aarch64/linux-stack-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_linux_stack', aarch64_linux_stack_test)

    aarch64_address_space_test = executable('aarch64_address_space_test', [
            'guest/memory/address-space.c',
            'guest/memory/address-space-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_address_space', aarch64_address_space_test)

    aarch64_tlb_test = executable('aarch64_tlb_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/memory/tlb-test.c',
        ], include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1'], dependencies: threads)
    test('aarch64_tlb', aarch64_tlb_test)

    aarch64_tlb_shared_test = executable('aarch64_tlb_shared_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/memory/page-backing.c',
            'guest/memory/tlb-shared-test.c',
        ], include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1'], dependencies: threads)
    test('aarch64_tlb_shared', aarch64_tlb_shared_test,
        timeout: 120)

    aarch64_page_backing_test = executable('aarch64_page_backing_test', [
            'guest/memory/page-backing.c',
            'guest/memory/page-backing-test.c',
        ], include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1',
            '-DGUEST_PAGE_BACKING_TESTING=1'], dependencies: threads)
    test('aarch64_page_backing', aarch64_page_backing_test)

    aarch64_page_table_test = executable('aarch64_page_table_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/memory/page-backing.c',
            'guest/memory/page-table.c',
            'guest/memory/page-table-test.c',
        ], include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1',
            '-DGUEST_PAGE_BACKING_TESTING=1',
            '-DGUEST_PAGE_TABLE_TESTING=1'])
    test('aarch64_page_table', aarch64_page_table_test)

    aarch64_runner_test = executable('aarch64_runner_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
        ] + aarch64_backend_src + [
            'guest/aarch64/runner-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_runner', aarch64_runner_test)

    aarch64_backend_test = executable('aarch64_backend_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
        ] + aarch64_backend_src + [
            'guest/aarch64/backend-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_backend', aarch64_backend_test)

    if host_machine.cpu_family() == 'aarch64'
        aarch64_backend_benchmark = executable(
            'aarch64_backend_benchmark',
            'guest/aarch64/backend-benchmark.c',
            include_directories: includes,
            c_args: ['-DISH_GUEST_AARCH64=1'],
            link_with: libish_aarch64_core)
        benchmark('aarch64_backend_performance',
            aarch64_backend_benchmark, timeout: 120)
    endif

    aarch64_load_store_test = executable('aarch64_load_store_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
        ] + aarch64_backend_src + [
            'guest/aarch64/load-store-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_load_store', aarch64_load_store_test)

    aarch64_ldpsw_test = executable('aarch64_ldpsw_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/ldpsw-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_ldpsw', aarch64_ldpsw_test)

    aarch64_program_test = executable('aarch64_program_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
        ] + aarch64_backend_src + [
            'guest/aarch64/linux-syscall.c',
            'guest/aarch64/program-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_program', aarch64_program_test)

    aarch64_e0_test = executable('aarch64_e0_test',
        'guest/aarch64/e0-test.c',
        include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1'],
        link_with: libish_aarch64_core)
    test('aarch64_e0', aarch64_e0_test)

    aarch64_instruction_test = executable('aarch64_instruction_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/instruction-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_instruction', aarch64_instruction_test)

    aarch64_data_processing_2source_test = executable(
        'aarch64_data_processing_2source_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/data-processing-2source-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_data_processing_2source',
        aarch64_data_processing_2source_test)

    aarch64_add_sub_extended_test = executable(
        'aarch64_add_sub_extended_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/add-sub-extended-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_add_sub_extended', aarch64_add_sub_extended_test)

    aarch64_add_sub_carry_test = executable(
        'aarch64_add_sub_carry_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/add-sub-carry-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_add_sub_carry', aarch64_add_sub_carry_test)

    aarch64_system_register_test = executable(
        'aarch64_system_register_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/system-register-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_system_register', aarch64_system_register_test)

    aarch64_multiply_add_test = executable('aarch64_multiply_add_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/multiply-add-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_multiply_add', aarch64_multiply_add_test)

    aarch64_extract_test = executable('aarch64_extract_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/extract-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_extract', aarch64_extract_test)

    aarch64_data_processing_1source_test = executable(
        'aarch64_data_processing_1source_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/data-processing-1source-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_data_processing_1source',
        aarch64_data_processing_1source_test)

    aarch64_advsimd_immediate_test = executable(
        'aarch64_advsimd_immediate_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/advsimd-immediate-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_advsimd_immediate', aarch64_advsimd_immediate_test)

    aarch64_advsimd_shift_long_test = executable(
        'aarch64_advsimd_shift_long_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/advsimd-shift-long-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_advsimd_shift_long', aarch64_advsimd_shift_long_test)

    aarch64_advsimd_copy_test = executable('aarch64_advsimd_copy_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/advsimd-copy-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_advsimd_copy', aarch64_advsimd_copy_test)

    aarch64_advsimd_add_test = executable('aarch64_advsimd_add_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/advsimd-add-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_advsimd_add', aarch64_advsimd_add_test)

    aarch64_advsimd_table_test = executable('aarch64_advsimd_table_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/advsimd-table-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_advsimd_table', aarch64_advsimd_table_test)

    aarch64_advsimd_permute_test = executable(
        'aarch64_advsimd_permute_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/advsimd-permute-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_advsimd_permute', aarch64_advsimd_permute_test)

    aarch64_advsimd_compare_test = executable(
        'aarch64_advsimd_compare_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/advsimd-compare-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_advsimd_compare', aarch64_advsimd_compare_test)

    aarch64_advsimd_logical_test = executable(
        'aarch64_advsimd_logical_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/advsimd-logical-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_advsimd_logical', aarch64_advsimd_logical_test)

    aarch64_advsimd_pairwise_test = executable(
        'aarch64_advsimd_pairwise_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/advsimd-pairwise-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_advsimd_pairwise', aarch64_advsimd_pairwise_test)

    aarch64_fmov_transfer_test = executable('aarch64_fmov_transfer_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/fmov-transfer-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_fmov_transfer', aarch64_fmov_transfer_test)

    aarch64_integer_to_fp_test = executable('aarch64_integer_to_fp_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/integer-to-fp-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_integer_to_fp', aarch64_integer_to_fp_test)

    aarch64_scalar_fp_decode_test = executable(
            'aarch64_scalar_fp_decode_test', [
            'guest/aarch64/decode.c',
            'guest/aarch64/scalar-fp-decode-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_scalar_fp_decode', aarch64_scalar_fp_decode_test)

    aarch64_scalar_fp_test = executable('aarch64_scalar_fp_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/scalar-fp-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_scalar_fp', aarch64_scalar_fp_test)

    aarch64_simd_pair_test = executable('aarch64_simd_pair_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/simd-pair-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_simd_pair', aarch64_simd_pair_test)

    aarch64_simd_load_store_test = executable(
        'aarch64_simd_load_store_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/simd-load-store-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_simd_load_store', aarch64_simd_load_store_test)

    aarch64_exclusive_test = executable('aarch64_exclusive_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/exclusive-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_exclusive', aarch64_exclusive_test)

    aarch64_shared_exclusive_test = executable(
        'aarch64_shared_exclusive_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/memory/page-backing.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/shared-exclusive-test.c',
        ], include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1'], dependencies: threads)
    test('aarch64_shared_exclusive', aarch64_shared_exclusive_test)

    aarch64_casp_test = executable('aarch64_casp_test',
        'guest/aarch64/casp-test.c',
        include_directories: includes,
        c_args: ['-DISH_GUEST_AARCH64=1'],
        dependencies: threads,
        link_with: libish_aarch64_core)
    test('aarch64_casp', aarch64_casp_test)

    aarch64_barrier_test = executable('aarch64_barrier_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/barrier-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_barrier', aarch64_barrier_test)
endif

e2e_test = find_program('tests/e2e/e2e.bash')
test('e2e', e2e_test, args: ['-y'], timeout: 180)
endif
