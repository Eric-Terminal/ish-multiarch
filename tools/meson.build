if get_option('kernel') == 'ish'
    # these tools are specific to x86_64 linux
    if host_machine.system() == 'linux' and host_machine.cpu() == 'x86_64'
        transplant_src = [
            'vdso-transplant.c',
            'ptutil.c',
        ]
        executable('ptraceomatic', ['ptraceomatic.c', 'undefined-flags.c', transplant_src], dependencies: [ish])
        configure_file(input: 'ptraceomatic-gdb.gdb', output: '@PLAINNAME@', copy: true)

        # tools for messing with vdsos
        # executable('vdso-dump', ['vdso-dump.c'], c_args: ['-m32'], link_args: ['-m32'])
        executable('vdso-transplant', ['vdso-transplant-main.c', transplant_src],
            include_directories: includes)
    endif

    unicorn = cc.find_library('unicorn', required: false)
    if unicorn.found()
        executable('unicornomatic', ['unicornomatic.c', 'undefined-flags.c'], dependencies: [ish, unicorn])
        configure_file(input: 'ptraceomatic-gdb.gdb', output: 'unicornomatic-gdb.gdb', copy: true)
    endif
endif

if not meson.is_cross_build()
    meson_cli = find_program('meson', required: false)
    if meson_cli.found()
        apple_meson_project_options_test = find_program(
            'apple-meson-project-options-test.sh')
        test('apple_meson_project_options',
            apple_meson_project_options_test,
            args: [meson_cli.full_path()], timeout: 30)
    else
        warning('未找到 Meson CLI，跳过旧项目选项刷新测试')
    endif

    apple_watch_package_test = find_program(
        'apple-watch-package-test.sh')
    test('apple_watch_package_prebuilt',
        apple_watch_package_test, timeout: 30)

    python3_cli = find_program('python3', required: false)
    if python3_cli.found()
        apple_aarch64_rootfs_sources_test = find_program(
            'apple-aarch64-rootfs-sources-test.sh')
        test('apple_aarch64_source_bundle',
            apple_aarch64_rootfs_sources_test,
            args: [python3_cli.full_path()], timeout: 60)
        apple_aarch64_rootfs_licenses_test = find_program(
            'apple-aarch64-rootfs-licenses-test.sh')
        test('apple_aarch64_rootfs_licenses',
            apple_aarch64_rootfs_licenses_test,
            args: [python3_cli.full_path()], timeout: 60)
        apple_aarch64_lgpl_surface_test = files(
            'apple-aarch64-lgpl-surface-test.py')
        test('apple_aarch64_lgpl_surface', python3_cli,
            args: apple_aarch64_lgpl_surface_test, timeout: 60)
        apple_license_notices_app_test = files(
            'apple-license-notices-app-test.py')
        test('apple_license_notices_app', python3_cli,
            args: apple_license_notices_app_test, timeout: 30)
        apple_host_delivery_inputs_test = files(
            'apple-host-delivery-inputs-test.py')
        test('apple_host_delivery_inputs', python3_cli,
            args: apple_host_delivery_inputs_test, timeout: 60)
        apple_host_link_closure_test = files(
            'apple-host-link-closure-test.py')
        test('apple_host_link_closure', python3_cli,
            args: apple_host_link_closure_test, timeout: 30)
        apple_host_notices_test = files(
            'apple-host-notices-test.py')
        test('apple_host_notices', python3_cli,
            args: apple_host_notices_test, timeout: 30)
        apple_project_license_notices_test = files(
            'apple-project-license-notices-test.py')
        test('apple_project_license_notices', python3_cli,
            args: apple_project_license_notices_test, timeout: 30)
    else
        warning('未找到 Python 3，跳过 AArch64 来源、许可与 Apple 宿主输入测试')
    endif
endif

libarchive = dependency('libarchive', required: false)
if not libarchive.found()
    # homebrew
    libarchive_lib = cc.find_library('archive', dirs: ['/usr/local/opt/libarchive/lib'], required: false)
    if libarchive_lib.found()
        libarchive = declare_dependency(dependencies: [libarchive_lib], include_directories: include_directories(['/usr/local/opt/libarchive/include']))
    else
        # Apple Silicon Homebrew
        libarchive_lib = cc.find_library('archive', dirs: ['/opt/homebrew/opt/libarchive/lib'], required: false)
        if libarchive_lib.found()
            libarchive = declare_dependency(dependencies: [libarchive_lib], include_directories: include_directories(['/opt/homebrew/opt/libarchive/include']))
        endif
    endif
endif

if libarchive.found()
    fakefsify_src = [
        'fakefsify.c',
        'fakefs.c',
        '../util/fchdir.c',
    ]
    fakefsify = executable('fakefsify', fakefsify_src, dependencies: [libarchive], link_with: [libfakefs], include_directories: [includes])
    sqlite3_cli = find_program('sqlite3', required: false)
    if not meson.is_cross_build() and sqlite3_cli.found()
        apple_aarch64_rootfs_test = find_program(
            'apple-aarch64-rootfs-test.sh')
        test('apple_aarch64_rootfs', apple_aarch64_rootfs_test,
            args: [fakefsify, sqlite3_cli.full_path()], timeout: 120)
    elif not meson.is_cross_build()
        warning('未找到 sqlite3 CLI，跳过 AArch64 rootfs 打包测试')
    endif
    custom_target('unfakefsify',
        build_by_default: true,
        command: ['ln', '-sf', 'fakefsify', '@OUTPUT@'],
        output: 'unfakefsify')
endif
