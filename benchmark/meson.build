# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

project(
    'poulsback-benchmarks',
    'c',
    default_options: ['c_std=c11', 'warning_level=2'],
)

cc = meson.get_compiler('c')
gl = dependency('gl')
x11 = dependency('x11')
egl = dependency('egl')
gles = dependency('glesv2')
glut = dependency('glut')
glu = dependency('glu')
math = cc.find_library('m')
dl = cc.find_library('dl')
rt = cc.find_library('rt', required: false)
glx = [gl, x11, math]
mesa = 'OpenGL/mesa/'
acceptance = '3D/gears/'
metrics_dir = '3D/metrics/'
headers = include_directories(
    'OpenGL/common/util',
    'OpenGL/common/glad/include',
    'OpenGL/common/eglut',
    '3D/metrics',
)
add_project_arguments('-D_GNU_SOURCE', language: 'c')

glad = static_library(
    'demo-glad',
    'OpenGL/common/glad/src/glad.c',
    include_directories: headers,
)
glad_dep = declare_dependency(link_with: glad, dependencies: dl)

foreach name : ['glxgears', 'glxheads']
    executable(
        name + '-8.5.0',
        acceptance + name + '.c',
        dependencies: glx,
        install: true,
        install_dir: 'bin/3D',
    )
endforeach

executable(
    'poulscube',
    '3D/poulscube/poulscube.c',
    c_args: ['-Werror'],
    dependencies: glx,
    install: true,
    install_dir: 'bin/3D',
)

executable(
    'glxinfo',
    [mesa + 'xdemos/glxinfo.c', 'OpenGL/common/util/glinfo_common.c'],
    include_directories: headers,
    dependencies: glx + [glad_dep],
    install: true,
    install_dir: 'bin/OpenGL',
)
executable(
    'sharedtex',
    mesa + 'xdemos/sharedtex.c',
    dependencies: glx,
    install: true,
    install_dir: 'bin/OpenGL',
)

metrics = configure_file(
    input: metrics_dir + 'glxgears.c',
    output: 'glxgears-metrics.c',
    copy: true,
)
run_command(
    find_program('patch'),
    '--batch',
    '--fuzz=0',
    metrics,
    files(metrics_dir + 'glxgears-metrics.patch'),
    check: true,
)
executable(
    'glxgears-metrics',
    [metrics, metrics_dir + 'frame_stats.c'],
    include_directories: headers,
    c_args: ['-Werror'],
    dependencies: glx + [rt],
    install: true,
    install_dir: 'bin/3D',
)

foreach name : [
    'tri-lit',
    'tri-clip',
    'tri-cull',
    'tri-flat',
    'drawarrays',
    'drawelements',
]
    executable(
        name,
        '3D/geometry/' + name + '.c',
        include_directories: headers,
        c_args: ['-DHAVE_FREEGLUT'],
        dependencies: glx + [glut, glu, glad_dep],
        install: true,
        install_dir: 'bin/3D',
    )
endforeach
foreach name : [
    'tri-tex',
    'quad-tex-sub',
    'tri-blend',
    'tri-mask-tri',
    'tri-scissor-tri',
    'tri-stencil',
    'tri-fbo-tex',
    'clear-fbo-scissor',
]
    executable(
        name,
        mesa + 'trivial/' + name + '.c',
        include_directories: headers,
        c_args: ['-DHAVE_FREEGLUT'],
        dependencies: glx + [glut, glu, glad_dep],
        install: true,
        install_dir: 'bin/OpenGL',
    )
endforeach

foreach name : ['es2_info', 'es2tri']
    executable(
        name,
        mesa + 'egl/opengles2/' + name + '.c',
        dependencies: [egl, gles, x11, math],
        install: true,
        install_dir: 'bin/OpenGL',
    )
endforeach
executable(
    'es2gears_x11',
    [
        mesa + 'egl/opengles2/es2gears.c',
        'OpenGL/common/eglut/eglut.c',
        'OpenGL/common/eglut/eglut_x11.c',
        'OpenGL/common/util/matrix.c',
    ],
    include_directories: headers,
    dependencies: [egl, gles, x11, math],
    install: true,
    install_dir: 'bin/OpenGL',
)

xext = dependency('xext')
xrender = dependency('xrender')
render_version = configuration_data()
render_version.set_quoted('VERSION', '1.6')
configure_file(output: 'version.h', configuration: render_version)
render = '2D/rendercheck/'
executable(
    'rendercheck',
    [
        render + 'main.c',
        render + 'ops.c',
        render + 'tests.c',
        render + 't_blend.c',
        render + 't_bug7366.c',
        render + 't_composite.c',
        render + 't_dstcoords.c',
        render + 't_fill.c',
        render + 't_gradient.c',
        render + 't_gtk_argb_xbgr.c',
        render + 't_libreoffice_xrgb.c',
        render + 't_repeat.c',
        render + 't_shmblend.c',
        render + 't_srccoords.c',
        render + 't_tsrccoords.c',
        render + 't_tsrccoords2.c',
        render + 't_triangles.c',
    ],
    c_args: cc.has_header('err.h') ? ['-DHAVE_ERR_H'] : [],
    dependencies: [x11, xext, xrender],
    install: true,
    install_dir: 'bin/2D',
)

xconf = configuration_data()
foreach name : [
    'inttypes.h',
    'stdint.h',
    'stdio.h',
    'stdlib.h',
    'strings.h',
    'string.h',
    'sys/stat.h',
    'sys/types.h',
    'unistd.h',
]
    xconf.set(
        'HAVE_' + name.to_upper().replace('.', '_').replace('/', '_'),
        cc.has_header(name) ? 1 : 0,
    )
endforeach
foreach name : [
    'STDC_HEADERS',
    'HAVE_XSHMQUERYEXTENSION',
    'MITSHM',
    'XFT',
    'XRENDER',
]
    xconf.set(name, 1)
endforeach
foreach name : ['PACKAGE', 'PACKAGE_NAME', 'PACKAGE_TARNAME']
    xconf.set_quoted(name, 'x11perf')
endforeach
foreach name : ['VERSION', 'PACKAGE_VERSION']
    xconf.set_quoted(name, '1.7.0')
endforeach
xconf.set_quoted('PACKAGE_STRING', 'x11perf 1.7.0')
xconf.set_quoted(
    'PACKAGE_BUGREPORT',
    'https://gitlab.freedesktop.org/xorg/test/x11perf/-/issues',
)
xconf.set_quoted('PACKAGE_URL', '')
xconf.set('PACKAGE_VERSION_MAJOR', 1)
xconf.set('PACKAGE_VERSION_MINOR', 7)
xconf.set('PACKAGE_VERSION_PATCHLEVEL', 0)
configure_file(output: 'config.h', configuration: xconf)
xperf = '2D/x11perf/'
executable(
    'x11perf',
    [
        xperf + 'bitmaps.c',
        xperf + 'do_arcs.c',
        xperf + 'do_blt.c',
        xperf + 'do_complex.c',
        xperf + 'do_dots.c',
        xperf + 'do_lines.c',
        xperf + 'do_movewin.c',
        xperf + 'do_rects.c',
        xperf + 'do_segs.c',
        xperf + 'do_simple.c',
        xperf + 'do_tests.c',
        xperf + 'do_text.c',
        xperf + 'do_traps.c',
        xperf + 'do_tris.c',
        xperf + 'do_valgc.c',
        xperf + 'do_windows.c',
        xperf + 'x11perf.c',
    ],
    c_args: ['-DHAVE_CONFIG_H'],
    dependencies: [
        x11,
        xext,
        xrender,
        dependency('xmuu'),
        dependency('xft'),
        dependency('fontconfig'),
        math,
    ],
    install: true,
    install_dir: 'bin/2D',
)
