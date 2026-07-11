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

aarch64_core_src = [
    'guest/memory/address-space.c',
    'guest/memory/tlb.c',
    'guest/memory/page-table.c',
    'guest/linux/user-memory.c',
    'guest/aarch64/decode.c',
    'guest/aarch64/condition.c',
    'guest/aarch64/execute.c',
    'guest/aarch64/runner.c',
    'guest/aarch64/linux-syscall.c',
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

if not get_option('core_only')
threads = dependency('threads')
librt = cc.find_library('rt', required: false)
libm = cc.find_library('m', required: false)
libdl = cc.find_library('dl', required: false)
sqlite3 = cc.find_library('sqlite3')
dependencies = [librt, libm, libdl, threads, sqlite3]

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
        'kernel/resource.c',
        'kernel/random.c',
        'kernel/misc.c',
        'kernel/eventfd.c',
        'kernel/ipc.c',
        'kernel/ptrace.c',

        'kernel/fs.c',
        'kernel/fs-task.c',
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
        include_directories: includes)
    ish = declare_dependency(
        link_with: [libish, libish_emu, libfakefs],
        dependencies: dependencies,
        include_directories: includes)

    if not meson.is_cross_build()
        executable('ish', ['main.c'], dependencies: ish)

        fd_task_test = executable('fd_task_test', 'fs/fd-task-test.c', dependencies: ish)
        test('fd_task', fd_task_test)

        fs_task_test = executable('fs_task_test', 'kernel/fs-task-test.c', dependencies: ish)
        test('fs_task', fs_task_test)
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
    # test for floating point library
    float80_test = executable('float80_test', ['emu/float80.c', 'emu/float80-test.c'], dependencies: [libm])
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

    aarch64_linux_runtime_test = executable('aarch64_linux_runtime_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/memory/page-table.c',
            'guest/linux/user-memory.c',
            'guest/aarch64/linux-syscall.c',
            'guest/linux/memory.c',
            'guest/aarch64/linux-runtime.c',
            'guest/aarch64/linux-runtime-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_linux_runtime', aarch64_linux_runtime_test)

    aarch64_linux_user_memory_test = executable(
        'aarch64_linux_user_memory_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/memory/page-table.c',
            'guest/linux/user-memory.c',
            'guest/linux/user-memory-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_linux_user_memory', aarch64_linux_user_memory_test)

    guest_linux_memory_test = executable('guest_linux_memory_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/memory/page-table.c',
            'guest/linux/memory.c',
            'guest/linux/memory-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('guest_linux_memory', guest_linux_memory_test)

    aarch64_elf64_test = executable('aarch64_elf64_test', [
            'guest/aarch64/elf64.c',
            'guest/aarch64/elf64-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_elf64', aarch64_elf64_test)

    aarch64_elf64_loader_test = executable('aarch64_elf64_loader_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/memory/page-table.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/runner.c',
            'guest/aarch64/linux-syscall.c',
            'guest/aarch64/elf64.c',
            'guest/aarch64/elf64-loader.c',
            'guest/aarch64/elf64-loader-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_elf64_loader', aarch64_elf64_loader_test)

    aarch64_linux_stack_test = executable('aarch64_linux_stack_test', [
            'guest/memory/address-space.c',
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
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_tlb', aarch64_tlb_test)

    aarch64_page_table_test = executable('aarch64_page_table_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/memory/page-table.c',
            'guest/memory/page-table-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_page_table', aarch64_page_table_test)

    aarch64_runner_test = executable('aarch64_runner_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/runner.c',
            'guest/aarch64/runner-test.c',
        ], include_directories: includes, c_args: ['-DISH_GUEST_AARCH64=1'])
    test('aarch64_runner', aarch64_runner_test)

    aarch64_load_store_test = executable('aarch64_load_store_test', [
            'guest/memory/address-space.c',
            'guest/memory/tlb.c',
            'guest/aarch64/decode.c',
            'guest/aarch64/condition.c',
            'guest/aarch64/execute.c',
            'guest/aarch64/runner.c',
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
            'guest/aarch64/runner.c',
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
