# Copyright (c) 2026, PostgreSQL Global Development Group
#
# End-to-end test: a standby taken before a same-version self-upgrade (the
# pg_upgrade test mode also used by pg_upgrade's own TAP suite) is rebuilt
# with pg_upgrade_replica against the upgraded primary, without ever
# re-cloning the dataset, and boots as a working standby of it.
#
# pg_upgrade_replica forges a backup_manifest and drives pg_basebackup
# --incremental + pg_combinebackup rather than fetching files itself, so
# the new primary needs summarize_wal=on from its very first startup (set
# below, before $new_primary->start) for the incremental backup protocol
# to have anything to work from.

use strict;
use warnings FATAL => 'all';

use File::Basename qw(dirname);
use File::Path     qw(rmtree);
use IPC::Run       qw(run);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Standbys categorically refuse SQL access to unlogged relations (their
# content is only reset at end of recovery, which a standby never
# reaches), so t1_unlogged's presence on the assembled standby can only
# be checked at the filesystem level below, not by querying it.

my $old_primary = PostgreSQL::Test::Cluster->new('old_primary');
$old_primary->init(allows_streaming => 1);

# Needed at startup, not just when creating the in-place tablespace
# below: any server (old_primary itself on its own later restart during
# pg_upgrade, old_standby entering recovery, ...) that finds one already
# on disk PANICs at startup without this set, not just refuses to create
# a new one.
$old_primary->append_conf('postgresql.conf',
	'allow_in_place_tablespaces = on');
$old_primary->start;

$old_primary->safe_psql('postgres',
		"create table t1 as select g, repeat('x', 80) as pad "
	  . "from generate_series(1, 5000) g;"
	  . "create index on t1(g);"
	  . "create unlogged table t1_unlogged as select g from generate_series(1, 10) g;"
);

my $t1_relpath =
  $old_primary->safe_psql('postgres', "select pg_relation_filepath('t1')");

# A real, non-default tablespace: exercises build_old_replica_view()'s
# version-dir-renaming symlink and forge_manifest.c's tablespace walk,
# neither of which the default-tablespace-only checks above can reach.
# In-place (allow_in_place_tablespaces, testing-only: data lives at
# pg_tblspc/<oid> inside the data directory itself) rather than a real
# external location, so this works under the same-version self-upgrade
# this whole test relies on -- pg_upgrade refuses a real external
# tablespace when old and new are the same catalog version, since it
# can't tell the two clusters' copies apart at that one shared path.
$old_primary->safe_psql('postgres',
		"create tablespace test_tblspc location '';"
	  . "create table t2 tablespace test_tblspc as select g, repeat('y', 80) as pad "
	  . "from generate_series(1, 3000) g;");
my $t2_oid = $old_primary->safe_psql('postgres',
	"select oid from pg_tablespace where spcname = 'test_tblspc'");
my $t2_relpath =
  $old_primary->safe_psql('postgres', "select pg_relation_filepath('t2')");

# No tablespace-specific backup/mapping options needed: an in-place
# tablespace is just a plain subdirectory of the data directory as far
# as pg_basebackup and copypath() are both concerned, unlike a real
# external one (which would need a -T/tablespace_map entry at each step
# below to relocate it).
$old_primary->backup('old_standby_backup');
my $old_standby = PostgreSQL::Test::Cluster->new('old_standby');
$old_standby->init_from_backup($old_primary, 'old_standby_backup',
	has_streaming => 1);
$old_standby->start;
$old_primary->wait_for_catchup($old_standby, 'replay');

is($old_standby->safe_psql('postgres', 'select count(*) from t1'),
	'5000', 'old standby caught up before upgrade');
is($old_standby->safe_psql('postgres', 'select count(*) from t2'),
	'3000', 'old standby caught up on the tablespace-resident table too');

# Correct shutdown order: primary, then standby, so the standby actually
# reaches the primary's final checkpoint before the upgrade runs.
$old_primary->stop;
$old_standby->stop;

# t1's frozen size on --old-replica, captured now while it's still just
# the original 5000 rows -- checked below against the *new* primary's own
# current (larger) size, to confirm the two genuinely differ going into
# the sync.
my $t1_frozen_size = -s ($old_standby->data_dir . '/' . $t1_relpath);
ok( defined $t1_frozen_size && $t1_frozen_size > 0,
	"captured t1's frozen file size on the old standby");

my $t2_frozen_size = -s ($old_standby->data_dir . '/' . $t2_relpath);
ok( defined $t2_frozen_size && $t2_frozen_size > 0,
	"captured t2's frozen file size on the old standby");

my $new_primary = PostgreSQL::Test::Cluster->new('new_primary');
$new_primary->init(allows_streaming => 1);

# Must be set before the new primary is ever started: the WAL summarizer
# only builds summaries forward from whenever it was actually enabled, so
# turning this on after the fact would leave a permanent gap starting at
# the new cluster's own checkpoint, exactly the range pg_basebackup
# --incremental needs summarized.
$new_primary->append_conf('postgresql.conf', 'summarize_wal = on');

# pg_upgrade doesn't carry postgresql.conf settings forward from the old
# cluster (a fresh initdb produced this one), so the in-place tablespace
# needs this set here too, before this cluster is ever started -- same
# PANIC-at-startup reasoning as old_primary/old_standby above.
$new_primary->append_conf('postgresql.conf',
	'allow_in_place_tablespaces = on');

my $bindir = $new_primary->config_data('--bindir');
command_ok(
	[
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $old_primary->data_dir,
		'--new-datadir' => $new_primary->data_dir,
		'--old-bindir' => $bindir,
		'--new-bindir' => $bindir,
		'--socketdir' => $new_primary->host,
		'--old-port' => $old_primary->port,
		'--new-port' => $new_primary->port,
	],
	'pg_upgrade self-upgrade for pg_upgrade_replica test');

$new_primary->start;

# A write to a *reused* relation, made on the new primary before
# pg_upgrade_replica ever runs, must still show up on the assembled
# standby: recovery is anchored at the new cluster's own checkpoint, not
# at whenever the sync happened to run. This is only a real test of that
# property if the write genuinely lands before the sync -- t1 keeps its
# relfilenode across the self-upgrade (default transfer mode), so it's a
# reused relation, and this insert has to happen in the gap between the
# primary starting and the sync running, not any later.
$new_primary->safe_psql('postgres',
	"insert into t1 values (99998, 'presync')");

# A write that actually changes t1's file length (not just an in-place
# page update, like the single-row insert above) must also still work,
# and now via a genuine incremental block fetch rather than either a
# whole-file reuse or a whole-file refetch.
$new_primary->safe_psql('postgres',
		"insert into t1 select g, repeat('x', 80) "
	  . "from generate_series(100000, 119999) g;");

my $t1_grown_size =
  $new_primary->safe_psql('postgres', "select pg_relation_size('t1')");
cmp_ok($t1_grown_size, '>', $t1_frozen_size,
	"t1 grew on the new primary past its frozen size on the old standby");

$new_primary->safe_psql('postgres',
		"insert into t2 select g, repeat('y', 80) "
	  . "from generate_series(100000, 109999) g;");
my $t2_grown_size =
  $new_primary->safe_psql('postgres', "select pg_relation_size('t2')");
cmp_ok($t2_grown_size, '>', $t2_frozen_size,
	"t2 grew on the new primary past its frozen size on the old standby");

my $unlogged_relpath = $new_primary->safe_psql('postgres',
	"select pg_relation_filepath('t1_unlogged')");

# An in-place tablespace created on the new primary after pg_upgrade ran
# has no counterpart at all on --old-replica's own pg_tblspc/<oid>
# (--old-replica is old_standby's pre-upgrade snapshot, taken before this
# tablespace ever existed): discover_tablespaces() must notice its
# in-place path is missing there and fall back to fetching it fresh via
# pg_basebackup, the same way it already does for a missing symlink
# target on a real external tablespace.
$new_primary->safe_psql('postgres',
		"create tablespace test_tblspc_new location '';"
	  . "create table t3 tablespace test_tblspc_new as select g, repeat('z', 80) as pad "
	  . "from generate_series(1, 2000) g;");
my $t3_oid = $new_primary->safe_psql('postgres',
	"select oid from pg_tablespace where spcname = 'test_tblspc_new'");
my $t3_relpath =
  $new_primary->safe_psql('postgres', "select pg_relation_filepath('t3')");

# A checkpoint gives the WAL summarizer a boundary to finalize a summary
# through, rather than waiting for the next automatic one on this
# cluster's default 5-minute checkpoint_timeout.
$new_primary->safe_psql('postgres', 'checkpoint');

my $new_replica = PostgreSQL::Test::Cluster->new('new_replica');

# check_old_replica_caught_up() (reuse.c) is the one thing standing between
# a lagging or plain-wrong --old-replica and silently trusting its reused
# files forever. Exercise its two refusal branches for real, against this
# same new primary/manifest, before relying on the happy-path run below to
# prove the tool works at all.
#
# A never-started cluster is already "cleanly shut down" (no
# postmaster.pid ever written), a cheap stand-in for "wrong replica for
# this upgrade" -- but only with force_initdb: a plain init() copies a
# shared template directory (see Cluster.pm's init(), INITDB_TEMPLATE)
# rather than running initdb for real, which would give this cluster the
# exact same system identifier as every other plain init() in this suite,
# including old_primary's.
my $bogus_replica = PostgreSQL::Test::Cluster->new('bogus_replica');
$bogus_replica->init(force_initdb => 1);

command_fails_like(
	[
		'pg_upgrade_replica',
		'--old-bindir' => $bindir,
		'--old-replica' => $bogus_replica->data_dir,
		'--new-replica' => $new_replica->data_dir . '_negtest',
		'--no-sync',
		'-h' => $new_primary->host,
		'-p' => $new_primary->port
	],
	qr/refusing to sync: old replica system identifier .* does not match/,
	'pg_upgrade_replica refuses a replica with the wrong system identifier');

# old_standby is genuinely caught up and cleanly stopped at this point (see
# above), so a fake postmaster.pid is the only thing standing between it
# and the refusal this checks for -- removed again right after, so the
# real run below still sees a cleanly-stopped replica.
my $fake_postmaster_pid = $old_standby->data_dir . '/postmaster.pid';
open(my $fh, '>', $fake_postmaster_pid)
  or die "could not create $fake_postmaster_pid: $!";
close $fh;
command_fails_like(
	[
		'pg_upgrade_replica',
		'--old-bindir' => $bindir,
		'--old-replica' => $old_standby->data_dir,
		'--new-replica' => $new_replica->data_dir . '_negtest',
		'--no-sync',
		'-h' => $new_primary->host,
		'-p' => $new_primary->port
	],
	qr/refusing to sync: ".*postmaster\.pid" exists/,
	'pg_upgrade_replica refuses an old replica that is not cleanly shut down'
);
unlink($fake_postmaster_pid)
  or die "could not remove $fake_postmaster_pid: $!";

# pg_basebackup --incremental checks WAL summary coverage against
# whatever it reads as "now" at the moment it actually runs, which the
# summarizer -- a background process working through its own backlog --
# is not guaranteed to have caught up to yet even right after a
# checkpoint (there's always a small, structural trailing gap between
# the summarizer's own pending and finalized positions). Retrying is the
# normal, expected remedy rather than a workaround for a bug: the same
# thing a real operator would do against the same transient error.
#
# This only exercises that transient case, never the terminal one
# (summarize_wal never enabled, or past wal_summary_keep_time), so
# pg_basebackup's own "fails plainly" behavior for that case is not
# covered here. Reaching it deterministically needs a second
# self-upgrade cycle with summarize_wal left off, real added setup cost
# for one error message.
my ($stdout, $stderr, $result);
my @cmd = (
	'pg_upgrade_replica',
	'--old-bindir' => $bindir,
	'--old-replica' => $old_standby->data_dir,
	'--new-replica' => $new_replica->data_dir,
	'--no-sync',
	'-h' => $new_primary->host,
	'-p' => $new_primary->port);
print("# Running: " . join(" ", @cmd) . "\n");
for (my $attempt = 1; $attempt <= 10; $attempt++)
{
	$result = run(\@cmd, '>' => \$stdout, '2>' => \$stderr);
	last
	  if $result
	  || $stderr !~ /WAL summaries .* are incomplete/;
	diag("WAL summarizer not caught up yet, retrying ($attempt/10)");
	rmtree($new_replica->data_dir) if -e $new_replica->data_dir;
	$new_primary->safe_psql('postgres', 'checkpoint');
	sleep 1;
}
ok($result,
	'pg_upgrade_replica syncs a standby against the upgraded primary');
diag($stderr);

like(
	$stderr,
	qr/manifest lists \d+ relation files as unchanged/,
	'sync reports the forged manifest\'s kept-relation count');

ok( -f $new_replica->data_dir . '/backup_label',
	'assembled directory has a backup_label');
ok( -f $new_replica->data_dir . '/standby.signal',
	'assembled directory has standby.signal');

# basebackup.c excludes every fork of an unlogged relation except the
# init fork from any base backup, full or incremental -- its main fork's
# content is meaningless until end-of-recovery resets it, which a
# standby never reaches, so pg_basebackup never sends it at all and it
# must NOT be present here. --old-replica has none either: an unlogged
# relation's main fork gets removed at the *standby's own* startup (not
# just at end-of-recovery/promotion), so forge_manifest.c's walk of
# --old-replica never finds one to list as kept in the first place.
ok( !-e $new_replica->data_dir . '/' . $unlogged_relpath,
	'assembled directory correctly has no main fork for the unlogged relation'
);
ok( -f $new_replica->data_dir . '/' . $unlogged_relpath . '_init',
	'assembled directory has the unlogged relation\'s init fork');

# An in-place tablespace's pg_tblspc/<oid> is a real directory, not a
# symlink to an external location -- pg_basebackup/pg_combinebackup
# place it there automatically, with no -T mapping from this tool.
ok( -d $new_replica->data_dir . "/pg_tblspc/$t2_oid"
	  && !-l $new_replica->data_dir . "/pg_tblspc/$t2_oid",
	'assembled directory has the in-place tablespace as a real directory');
ok(-f $new_replica->data_dir . '/' . $t2_relpath,
	'assembled directory has the tablespace-resident table\'s file');

# test_tblspc_new (created on the new primary after the upgrade, so
# --old-replica has no pg_tblspc/<oid> for it at all) must still come
# through, fetched fresh rather than causing the run above to fail.
ok( -d $new_replica->data_dir . "/pg_tblspc/$t3_oid"
	  && !-l $new_replica->data_dir . "/pg_tblspc/$t3_oid",
	'assembled directory has the post-upgrade in-place tablespace as a real directory'
);
ok( -f $new_replica->data_dir . '/' . $t3_relpath,
	'assembled directory has the post-upgrade tablespace-resident table\'s file'
);

$new_replica->append_conf('postgresql.auto.conf',
	'port = ' . $new_replica->port);
$new_replica->start;

is($new_replica->safe_psql('postgres', 'select pg_is_in_recovery()'),
	't', 'assembled standby is in recovery');
is($new_replica->safe_psql('postgres', 'select count(*) from t1'),
	'25001', 'assembled standby has the correct data');
is( $new_replica->safe_psql('postgres', 'select count(*) from t2'),
	'13000',
	'assembled standby has the correct data for the tablespace-resident table'
);
is( $new_replica->safe_psql('postgres', 'select count(*) from t3'),
	'2000',
	'assembled standby has the correct data for the post-upgrade tablespace-resident table'
);

# Checked before any catchup wait: these rows were already in the WAL by
# the time the standby was assembled, so they must be present from replay
# alone, not because streaming caught up afterward.
is( $new_replica->safe_psql('postgres', 'select pad from t1 where g = 99998'),
	'presync',
	'a write made before the sync ran still replays onto the standby');
is( $new_replica->safe_psql(
		'postgres',
		'select count(*) from t1 where g between 100000 and 119999'),
	'20000',
	'a write that grew a reused file before the sync ran still replays onto the standby'
);

# Separately, live streaming after boot must keep working normally: a
# fresh write made now, well after the standby is already caught up.
$new_primary->safe_psql('postgres',
	"insert into t1 values (99999, 'poststream')");
$new_primary->wait_for_catchup($new_replica, 'replay');
is( $new_replica->safe_psql('postgres', 'select pad from t1 where g = 99999'),
	'poststream',
	'live streaming after sync delivers a fresh write');

$new_replica->stop;
$new_primary->stop;

done_testing();
