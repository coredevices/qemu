decorations = files(
  'pt2-sb.png',
  'pt2-br.png',
  'pr2-bk20.png',
  'pr2-gd14.png',
  'p2d-bk.png',
  'p2d-wh.png',
)

if get_option('install_blobs')
  install_data(decorations,
               install_dir: qemu_datadir / 'pebble-decorations',
               install_mode: 'rw-r--r--')
endif
