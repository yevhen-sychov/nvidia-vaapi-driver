project(
    'nvidia-vaapi-driver',
    'c',
    version: '0.1',
    default_options: [
        'prefix=/usr',
        'c_std=c11',
        'warning_level=2',
    ],
    license: 'MIT',
    meson_version: '>= 0.58.0',
)

cc = meson.get_compiler('c')

deps = [
    cc.find_library('m'),
    cc.find_library('dl', required : false),
    dependency('egl'),
    dependency('ffnvcodec', version: '>= 11.1.5.1'),
    dependency('libdrm', version: '>=2.4.60').partial_dependency(compile_args: true),
    dependency('threads'),
]
libva_deps = dependency('libva', version: '>= 1.8.0').partial_dependency(compile_args: true)
deps += [libva_deps]

gst_codecs_deps = dependency('gstreamer-codecparsers-1.0', required: false)

if cc.get_argument_syntax() == 'gcc'
    add_project_arguments(
        cc.get_supported_arguments([
            '-Wno-missing-field-initializers',
            '-Wno-unused-parameter',
            '-Werror=format',
            '-Werror=format-security',
            '-Werror=incompatible-pointer-types',
            '-Werror=init-self',
            '-Werror=int-conversion',
            '-Werror=missing-declarations',
            '-Werror=missing-prototypes',
            '-Werror=pointer-arith',
            '-Werror=undef',
            '-Werror=vla',
            '-Wsuggest-attribute=format',
            '-Wwrite-strings',
        ]),
        language: ['c'],
    )
endif

sources = [
    'src/av1.c',
    'src/backend-common.c',
    'src/export-buf.c',
    'src/direct/direct-export-buf.c',
    'src/direct/nv-driver.c',
    'src/h264.c',
    'src/h264_encode.c',
    'src/hevc.c',
    'src/hevc_encode.c',
    'src/av1_encode.c',
    'src/jpeg.c',
    'src/kernels.c',
    'src/mpeg2.c',
    'src/mpeg4.c',
    'src/nvenc.c',
    'src/nvenc_dispatch.c',
    'src/nvenc-ipc-client.c',
    'src/stats.c',
    'src/vabackend.c',
    'src/vc1.c',
    'src/vp8.c',
    'src/list.c',
]

if gst_codecs_deps.found()
    sources += ['src/vp9.c']
    deps += [gst_codecs_deps]
endif

nvidia_incdir = include_directories('nvidia-include')
nvidia_install_dir = libva_deps.get_variable(pkgconfig: 'driverdir')

shared_library(
    'nvidia_drv_video',
    name_prefix: '',
    sources: sources,
    dependencies: deps,
    include_directories: nvidia_incdir,
    install: true,
    install_dir: nvidia_install_dir,
    gnu_symbol_visibility: 'hidden',
)

# Build the 64-bit NVENC helper daemon (only for native builds, not cross-compiled i386)
if host_machine.cpu_family() == 'x86_64' or host_machine.cpu_family() == 'aarch64'
    helper_deps = [
        cc.find_library('dl', required : false),
        dependency('ffnvcodec', version: '>= 11.1.5.1'),
        dependency('threads'),
    ]
    executable(
        'nvenc-helper',
        'src/nvenc-helper.c',
        'src/nvenc-ipc-client.c',   # for nvenc_ipc_get_socket_path
        dependencies: helper_deps,
        install: true,
        install_dir: get_option('libexecdir'),
    )
endif

# Tests (native builds only, not cross-compiled)
if not meson.is_cross_build()
    libva_test_deps = [
        dependency('libva'),
        dependency('libva-drm'),
        dependency('libdrm'),
        cc.find_library('m', required : false),
    ]

    test_env = environment({
        'NVD_LOG': '1',
        'LIBVA_DRIVER_NAME': 'nvidia',
        'LIBVA_DRIVERS_PATH': meson.project_build_root(),
        'LD_LIBRARY_PATH': meson.project_build_root(),
    })

    test_encode = executable('test_encode', 'tests/test_encode.c',
        dependencies : libva_test_deps, install : false)
    # Encoder tests hammer the NVENC engine hard — run them serially
    # (is_parallel:false) so they don't contend with each other for
    # concurrent-session capacity on consumer cards. Same reason for
    # encode_roundtrip below.
    test('encode', test_encode, timeout : 60, env : test_env,
         is_parallel : false)

    test_encode_config = executable('test_encode_config', 'tests/test_encode_config.c',
        dependencies : libva_test_deps, install : false)
    test('encode_config', test_encode_config, timeout : 60, env : test_env)

    test_decode = executable('test_decode', 'tests/test_decode.c',
        dependencies : libva_test_deps, install : false)
    test('decode', test_decode, timeout : 60, env : test_env)

    test_descriptor_mode = executable('test_descriptor_mode',
        'tests/test_descriptor_mode.c',
        dependencies : libva_test_deps, install : false)
    test('descriptor_mode', test_descriptor_mode, timeout : 60, env : test_env)

    # Low-level VA-API contracts that Chromium/Firefox/FFmpeg depend on --
    # easy to break while refactoring, and they fail downstream in confusing
    # ways (silent software fallback, or a browser-side abort) rather than here.
    # VideoProc blit geometry. Chromium's VaapiImageProcessorBackend asks for
    # NV12->ARGB with a source rectangle and a different output size whenever
    # the decoded surface cannot be imported into EGL directly; rejecting those
    # requests drops the whole stream to software decode.
    test_vpp = executable('test_vpp', 'tests/test_vpp.c',
        dependencies : libva_test_deps, install : false)
    test('vpp', test_vpp, timeout : 60, env : test_env)

    test_client_contracts = executable('test_client_contracts',
        'tests/test_client_contracts.c',
        dependencies : libva_test_deps, install : false)
    test('client_contracts', test_client_contracts, timeout : 60, env : test_env)

    test_concurrent = executable('test_concurrent_sessions',
        'tests/test_concurrent_sessions.c',
        dependencies : libva_test_deps + [dependency('threads')],
        install : false)
    test('concurrent_sessions', test_concurrent, timeout : 120, env : test_env)

    # Opt-in coverage: re-runs descriptor_mode with the escape hatch active
    # to pin the fallback behavior we promise in the README.
    test_env_selfpreview = environment({
        'NVD_LOG': '1',
        'LIBVA_DRIVER_NAME': 'nvidia',
        'LIBVA_DRIVERS_PATH': meson.project_build_root(),
        'LD_LIBRARY_PATH': meson.project_build_root(),
        'NVD_SELF_PREVIEW_COMBINED': '1',
    })
    test('descriptor_mode_self_preview_optin', test_descriptor_mode,
         timeout : 60, env : test_env_selfpreview)

    # MULTI is the only layout that exports one DMA-BUF object per plane, so
    # it is the only one that can surface a per-plane block-linear modifier
    # split to the client. Re-run the suite there to keep Chromium's
    # uniform-modifier CHECK covered on that path.
    test_env_multi = environment({
        'NVD_LOG': '1',
        'LIBVA_DRIVER_NAME': 'nvidia',
        'LIBVA_DRIVERS_PATH': meson.project_build_root(),
        'LD_LIBRARY_PATH': meson.project_build_root(),
        'NVD_DESCRIPTOR_MODE': 'multi',
    })
    test('descriptor_mode_multi', test_descriptor_mode,
         timeout : 60, env : test_env_multi)

    gst_launch = find_program('gst-launch-1.0', required : false)
    if gst_launch.found()
        test_env.set('GST_VAAPI_ALL_DRIVERS', '1')
        test('gstreamer', find_program('tests/test_gstreamer.sh'),
             timeout : 120,
             env : test_env)
    endif

    ffmpeg = find_program('ffmpeg', required : false)
    if ffmpeg.found()
        test('ffmpeg', find_program('tests/test_ffmpeg.sh'),
             args: [meson.current_source_dir() + '/samples/smptebars_h264.mp4'],
             timeout : 120,
             env : test_env)

        # Roundtrip: encode via our VA-API driver, decode via software,
        # measure PSNR. Catches bitstream corruption bugs that pass an
        # "encode succeeded" check but produce garbage frames — e.g.
        # missing NV_ENC_CONFIG.chromaFormatIDC. Uses libx264 (bundled
        # with ffmpeg) to generate a high-detail testsrc2 stress fixture
        # at runtime.
        test('encode_roundtrip',
             find_program('tests/test_encode_roundtrip.sh'),
             args: [meson.current_source_dir() + '/samples/smptebars_h264.mp4'],
             timeout : 180,
             env : test_env,
             is_parallel : false)

        # VP8 decode correctness. VA-API hands the driver slice data that
        # starts *after* the VP8 uncompressed data chunk, which NVDEC still
        # needs, so the driver has to reconstruct it. Compares a hardware
        # decode against libvpx and requires them to agree; generates its
        # own fixture (there is no checked-in VP8 sample).
        test('vp8_decode',
             find_program('tests/test_vp8_decode.sh'),
             timeout : 120,
             env : test_env)

        # The encode log must stay bounded by session setup rather than
        # growing with frame count -- per-frame chatter buries the events
        # that matter (resolution/bitrate changes, reconfigures, errors).
        # Sets NVD_LOG itself, so it does not inherit test_env's NVD_LOG=1.
        test('log_verbosity',
             find_program('tests/test_log_verbosity.sh'),
             timeout : 120,
             env : {
                 'LIBVA_DRIVER_NAME': 'nvidia',
                 'LIBVA_DRIVERS_PATH': meson.project_build_root(),
                 'LD_LIBRARY_PATH': meson.project_build_root(),
             },
             is_parallel : false)
    endif
endif

meson.add_devenv(environment({
    'NVD_LOG': '1',
    'LIBVA_DRIVER_NAME': 'nvidia',
    'LIBVA_DRIVERS_PATH': meson.project_build_root(),
}))
