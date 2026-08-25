# Copyright © 2018, 2019 Iñigo Martínez
# Copyright © 2019 Christian Persch
#
# This library is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this library.  If not, see <https://www.gnu.org/licenses/>.

app_sources = config_sources + debug_sources + glib_glue_sources + libc_glue_sources + pango_glue_sources + pcre2_glue_sources + refptr_sources + std_glue_sources + vte_glue_sources + files(
  'app.cc',
)

app_common_cppflags = [
  '-DG_LOG_DOMAIN="VTE"',
  '-DLOCALEDIR="@0@"'.format(vte_prefix / vte_localedir),
  '-DVTE_DISABLE_DEPRECATION_WARNINGS',
]

app_common_deps = [
  fmt_dep,
  glib_dep,
  pcre2_dep,
]

if get_option('gtk3')

  app_gtk3_resource_data = files(
    'appmenu-gtk3.ui',
    'search-popover-gtk3.ui',
    'window-gtk3.ui',
  )

  app_gtk3_resource_sources = gnome.compile_resources(
    'resources-gtk3.cc',
    'app-gtk3.gresource.xml',
    c_name: 'app',
    dependencies: app_gtk3_resource_data,
  )

  app_gtk3_sources = app_sources + app_gtk3_resource_sources
  app_gtk3_cppflags = app_common_cppflags + gtk3_version_cppflags + ['-DVTE_GTK=3',]
  app_gtk3_deps = app_common_deps + [libvte_gtk3_dep]

  app_gtk3 = executable(
    'vte-' + vte_api_version,
    app_gtk3_sources,
    dependencies: app_gtk3_deps,
    cpp_args: app_gtk3_cppflags,
    include_directories: top_inc,
    install: not meson.is_subproject(),
  )
endif

if get_option('gtk4')

  app_gtk4_resource_data = files(
    'appmenu-gtk4.ui',
    'search-popover-gtk4.ui',
    'window-gtk4.ui',
  )

  app_gtk4_resource_sources = gnome.compile_resources(
    'resources-gtk4.cc',
    'app-gtk4.gresource.xml',
    c_name: 'app',
    dependencies: app_gtk4_resource_data,
  )

  app_gtk4_sources = app_sources + [app_gtk4_resource_sources,]
  app_gtk4_cppflags = app_common_cppflags + gtk4_version_cppflags + ['-DVTE_GTK=4',]
  app_gtk4_deps = app_common_deps + [libvte_gtk4_dep]

  app_gtk4 = executable(
    'vte-' + vte_api_version + '-gtk4',
    app_gtk4_sources,
    dependencies: app_gtk4_deps,
    cpp_args: app_gtk4_cppflags,
    include_directories: top_inc,
    install: not meson.is_subproject(),
  )
endif

# Desktop files

desktopdatadir = vte_datadir / 'applications'

if get_option('gtk3')
  conf = configuration_data()
  conf.set('name', 'VTE Terminal for GTK3')
  conf.set('exec', vte_gtk3_api_name)
  conf.set('gtk', '3')

  app_gtk3_desktop_name = 'org.gnome.Vte.App.Gtk3.desktop'

  app_gtk3_desktop = configure_file(
    configuration: conf,
    input: 'vte.desktop.in',
    install: true,
    install_dir: vte_prefix / desktopdatadir,
    install_tag: 'runtime',
    output: app_gtk3_desktop_name,
  )

  meson.add_install_script(
    'meson_desktopfile.py',
    desktopdatadir,
    get_option('app-hidden').to_string(),
    app_gtk3_desktop_name,
  )
endif

if get_option('gtk4')
  conf = configuration_data()
  conf.set('name', 'VTE Terminal for GTK4')
  conf.set('exec', vte_gtk4_api_name)
  conf.set('gtk', '4')

  app_gtk4_desktop_name = 'org.gnome.Vte.App.Gtk4.desktop'

  app_gtk4_desktop = configure_file(
    configuration: conf,
    input: 'vte.desktop.in',
    install: true,
    install_dir: vte_prefix / desktopdatadir,
    install_tag: 'runtime',
    output: app_gtk4_desktop_name,
  )

  meson.add_install_script(
    'meson_desktopfile.py',
    desktopdatadir,
    get_option('app-hidden').to_string(),
    app_gtk4_desktop_name,
  )
endif

# Install a symlink for xdg-terminal-exec

if get_option('gtk3')
  install_symlink(
    app_gtk3_desktop_name,
    install_dir: vte_datadir / 'xdg-terminals',
    install_tag: 'runtime',
    pointing_to: '..' / 'applications' / app_gtk3_desktop_name,
  )
endif

if get_option('gtk4')
  install_symlink(
    app_gtk4_desktop_name,
    install_dir: vte_datadir / 'xdg-terminals',
    install_tag: 'runtime',
    pointing_to: '..' / 'applications' / app_gtk4_desktop_name,
  )
endif

# SIXEL golden-frame render tests.
#
# These assert on PIXELS, which is the only place most image properties are
# observable: that an image is drawn at all, at the right size and place, with
# the right colours, clipped where it should be. The GTK4 draw arm was absent
# for two years and no unit test noticed, because no unit test can.
#
# Run on every enabled arm, not just GTK4: the two draw the image through
# different backends - a cairo surface against a GdkTexture - so a build with
# only GTK3 enabled was the one running no draw-path test at all. The goldens
# are per arm, since the toolkits offset the terminal differently inside the
# captured window; see render-test.sh.
#
# The runner exits 77 (skip) when Xvfb or ImageMagick are absent, so a
# sandboxed build without a display reports "skipped" rather than a failure it
# could not tell apart from a real one.
if get_option('sixel')
  sixel_render_cases = [
    'bands',
    # Same image with a DECGCH spliced in. A command a VT340 does not
    # implement must not displace the rest of the image, so its golden is
    # byte-identical to bands' BY DESIGN.
    'bands-gch',
    # Emitted so it overhangs the right margin. DEC STD 070 11.2.2: sixels
    # past the right margin are not printed.
    'bands-margin',
    # Selects VT340 palette registers without defining them, with shared
    # colour registers, which is the path nothing initialised.
    'undefined-registers',
    # DECSET 8452: cursor to the right of the image on its last row, rather
    # than below it. The two cases must NOT render alike.
    'cursor-right-on',
    'cursor-right-off',
    # An oversized DECGRA raster. Transparent (P2=1) must occupy only the
    # rows its data needs; opaque must honour the declared raster.
    'raster-transparent',
    'raster-opaque',
    # A sixel cut off mid-stream, with no terminator. What arrived must still
    # be drawn - the top five bands of the eight - which is the xterm #323
    # behaviour vte.cc implements in the eos arm of its parser ABORT. Run with
    # the child exiting rather than sleeping; see the <case>.eof note in
    # render-test.sh.
    'bands-truncated',
  ]

  # The case the isolation test runs, which cannot be sixel_render_cases[0].
  #
  # That case is 'bands', and bands is font-invariant: measured on gtk3 it is
  # byte-identical at Monospace 12, Monospace 30 and DejaVu Serif 12, because
  # the image is drawn at its own pixel size at the terminal origin. The
  # isolation test's fontconfig probe has nothing to go red for on such a
  # case. bands-margin crops to the right margin, which sits at the column
  # count times the cell width, so it does move with the font - measured
  # AE=1764 under a fontconfig that reassigns Monospace to DejaVu Serif.
  sixel_isolation_case = 'bands-margin'

  sixel_test_apps = []

  if get_option('gtk3')
    sixel_test_apps += [['gtk3', app_gtk3,],]
  endif # gtk3

  if get_option('gtk4')
    sixel_test_apps += [['gtk4', app_gtk4,],]
  endif # gtk4

  sixel_test_dir = meson.project_source_root() / 'src' / 'tests' / 'sixel'

  foreach arm: sixel_test_apps
    test(
      'sixel-xtsmgraphics-' + arm[0],
      files(sixel_test_dir / 'xtsmgraphics-test.sh'),
      args: [arm[1].full_path()],
      depends: [arm[1]],
      timeout: 120,
    )

    # The render cases above are only worth their runtime if their verdict
    # tells a right frame from a wrong one. This one damages the captured frame
    # in each of the ways it can be wrong - one pixel, every pixel, one column
    # short - and requires the runner to reject each while still passing the
    # frame it captured untouched.
    test(
      'sixel-render-gate-' + arm[0],
      files(sixel_test_dir / 'render-gate-test.sh'),
      args: [
        arm[1].full_path(),
        sixel_test_dir,
        sixel_render_cases[0],
        arm[0],
      ],
      depends: [arm[1]],
      timeout: 240,
    )

    # And they are only worth their runtime if they measure the code rather
    # than whoever ran them. The runner keeps the developer's settings out of
    # the frame with two guards - a scratch config home, and --no-load-config -
    # and this one probes each SEPARATELY, so deleting either alone turns it
    # red with its own message: a user fontconfig for the scratch home, a
    # vteapp.ini forced back onto the app's own config path for the flag.
    # Each probe is preceded by a run showing the file it plants is not inert.
    test(
      'sixel-render-isolation-' + arm[0],
      files(sixel_test_dir / 'render-isolation-test.sh'),
      args: [
        arm[1].full_path(),
        sixel_test_dir,
        sixel_isolation_case,
        arm[0],
      ],
      depends: [arm[1]],
      timeout: 420,
    )

    foreach case: sixel_render_cases
      test(
        'sixel-render-' + case + '-' + arm[0],
        files(sixel_test_dir / 'render-test.sh'),
        args: [
          arm[1].full_path(),
          sixel_test_dir,
          case,
          arm[0],
        ],
        depends: [arm[1]],
        timeout: 120,
      )
    endforeach
  endforeach
endif
