/*-------------------------------------------------------------------------
 *
 * pg_upgrade_replica.c
 *		Rebuilds a standby's data directory against an already
 *		pg_upgrade'd, running primary, without a full re-clone. Forges a
 *		backup_manifest listing every relation pg_upgrade's own manifest
 *		says it transferred unchanged, anchored at the new cluster's own
 *		checkpoint, then drives pg_basebackup --incremental and
 *		pg_combinebackup from it: only what actually changed since that
 *		checkpoint is ever fetched over the wire, and --old-replica's own
 *		copy of everything else is reused in place. No ssh/rsync, and no
 *		filesystem access to the primary at all.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/bin/pg_upgrade_replica/pg_upgrade_replica.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "common/logging.h"
#include "fe_utils/option_utils.h"
#include "getopt_long.h"
#include "port.h"

#include "fetch.h"
#include "pg_upgrade_replica.h"

static void
usage(const char *progname)
{
	printf(_("%s rebuilds a standby's data directory against an upgraded primary.\n\n"), progname);
	printf(_("Usage:\n  %s [OPTION]...\n\n"), progname);
	printf(_("Options:\n"));
	printf(_("      --old-bindir=DIRECTORY      directory containing the old replica's own\n"
			 "                                  pg_controldata (its version must match the\n"
			 "                                  data in --old-replica)\n"));
	printf(_("      --old-replica=DIRECTORY     the standby's own pre-upgrade data directory\n"));
	printf(_("      --new-replica=DIRECTORY     directory to assemble the new standby into\n"
			 "                                  (must not exist or be empty)\n"));
	printf(_("      --link                      hardlink reused files instead of copying them\n"));
	printf(_("      --tablespace-mapping=OLDDIR=NEWDIR\n"
			 "                                  relocate the tablespace at OLDDIR (as reported\n"
			 "                                  by the new primary) to NEWDIR\n"
			 "                                  (may be given more than once)\n"));
	printf(_("      --no-sync                   do not wait for changes to be written\n"
			 "                                  safely to disk\n"));
	printf(_("  -V, --version                   output version information, then exit\n"));
	printf(_("  -?, --help                      show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME             new primary's host\n"));
	printf(_("  -p, --port=PORT                 new primary's port\n"));
	printf(_("  -U, --username=USERNAME         connect as this user\n"));
	printf(_("  -d, --dbname=DBNAME             database to connect to (default: postgres)\n"));
	printf(_("  -w, --no-password               never prompt for password\n"));
	printf(_("  -W, --password                  force password prompt\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

/*
 * Splits "OLDDIR=NEWDIR" the same way pg_basebackup's own
 * tablespace_list_append() does: the first unescaped '=' is the
 * separator, "\=" is a literal '=' inside either directory.
 */
static void
add_tablespace_mapping(SyncOptions *opts, const char *arg)
{
	TablespaceMapping *m;
	char		old_dir[MAXPGPATH] = {0};
	char		new_dir[MAXPGPATH] = {0};
	char	   *dst,
			   *dst_ptr;
	const char *arg_ptr;

	dst_ptr = dst = old_dir;
	for (arg_ptr = arg; *arg_ptr; arg_ptr++)
	{
		if (dst_ptr - dst >= MAXPGPATH)
			pg_fatal("directory name too long");

		if (*arg_ptr == '\\' && *(arg_ptr + 1) == '=')
			;					/* skip backslash escaping = */
		else if (*arg_ptr == '=' && (arg_ptr == arg || *(arg_ptr - 1) != '\\'))
		{
			if (*new_dir)
				pg_fatal("multiple \"=\" signs in tablespace mapping");
			else
				dst = dst_ptr = new_dir;
		}
		else
			*dst_ptr++ = *arg_ptr;
	}

	if (!*old_dir || !*new_dir)
		pg_fatal("invalid --tablespace-mapping \"%s\", expected OLDDIR=NEWDIR", arg);

	if (!is_absolute_path(old_dir))
		pg_fatal("old directory is not an absolute path in tablespace mapping: %s",
				 old_dir);
	if (!is_absolute_path(new_dir))
		pg_fatal("new directory is not an absolute path in tablespace mapping: %s",
				 new_dir);

	m = pg_malloc(sizeof(TablespaceMapping));
	m->old_dir = pg_strdup(old_dir);
	m->new_dir = make_absolute_path(new_dir);
	m->next = opts->tablespace_mappings;
	opts->tablespace_mappings = m;
}

int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"old-replica", required_argument, NULL, 1},
		{"new-replica", required_argument, NULL, 2},
		{"link", no_argument, NULL, 3},
		{"tablespace-mapping", required_argument, NULL, 4},
		{"no-sync", no_argument, NULL, 5},
		{"old-bindir", required_argument, NULL, 6},
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"dbname", required_argument, NULL, 'd'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},
		{NULL, 0, NULL, 0}
	};
	const char *progname;
	int			c,
				option_index;
	SyncOptions opts = {0};
	ConnParams	cparams = {0};
	RemoteConn *rconn;
	int			check_dir;

	pg_logging_init(argv[0]);
	pg_logging_set_level(PG_LOG_INFO);
	progname = get_progname(argv[0]);
	handle_help_version_opts(argc, argv, progname, usage);

	cparams.dbname = "postgres";
	cparams.prompt_password = TRI_DEFAULT;

	while ((c = getopt_long(argc, argv, "h:p:U:d:wW", long_options,
							&option_index)) != -1)
	{
		switch (c)
		{
			case 1:
				opts.old_replica = make_absolute_path(optarg);
				break;
			case 2:
				opts.new_replica = make_absolute_path(optarg);
				break;
			case 3:
				opts.link = true;
				break;
			case 4:
				add_tablespace_mapping(&opts, optarg);
				break;
			case 5:
				opts.no_sync = true;
				break;
			case 6:
				opts.old_bindir = make_absolute_path(optarg);
				break;
			case 'h':
				cparams.pghost = pg_strdup(optarg);
				break;
			case 'p':
				cparams.pgport = pg_strdup(optarg);
				break;
			case 'U':
				cparams.pguser = pg_strdup(optarg);
				break;
			case 'd':
				cparams.dbname = pg_strdup(optarg);
				break;
			case 'w':
				cparams.prompt_password = TRI_NO;
				break;
			case 'W':
				cparams.prompt_password = TRI_YES;
				break;
			default:
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (opts.old_bindir == NULL)
	{
		pg_log_error("--old-bindir is required");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}
	if (opts.old_replica == NULL)
	{
		pg_log_error("--old-replica is required");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}
	if (opts.new_replica == NULL)
	{
		pg_log_error("--new-replica is required");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * this tool has no resume/merge logic: a partial or stale attempt left in
	 * --new-replica would silently combine with this run. Just validate for
	 * now -- actually creating the directory is deferred to sync_replica(),
	 * once the old replica is confirmed caught up, so a rejected run doesn't
	 * leave a stray empty directory behind.
	 */
	check_dir = pg_check_dir(opts.new_replica);
	if (check_dir < 0)
		pg_fatal("could not access directory \"%s\": %m", opts.new_replica);
	if (check_dir > 1)
		pg_fatal("refusing to run: \"%s\" already exists and is not empty -- "
				 "remove it (or point --new-replica elsewhere) and retry",
				 opts.new_replica);

	rconn = remote_connect(&cparams, progname);
	pg_log_info("new primary data_directory = %s", rconn->data_directory);

	sync_replica(rconn, &cparams, &opts, argv[0]);

	remote_disconnect(rconn);
	return 0;
}
