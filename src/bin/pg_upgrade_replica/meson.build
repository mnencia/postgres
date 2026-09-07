# Copyright (c) 2022-2026, PostgreSQL Global Development Group

pg_upgrade_replica_sources = files(
  'fetch.c',
  'forge_manifest.c',
  'manifest.c',
  'pg_upgrade_replica.c',
  'reuse.c',
  'subprocess.c',
)

if host_system == 'windows'
  pg_upgrade_replica_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_upgrade_replica',
    '--FILEDESC', 'pg_upgrade_replica - rebuild a standby after pg_upgrade'])
endif

pg_upgrade_replica = executable('pg_upgrade_replica',
  pg_upgrade_replica_sources,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args,
)
bin_targets += pg_upgrade_replica

tests += {
  'name': 'pg_upgrade_replica',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
      't/002_sync.pl',
    ],
  },
}
