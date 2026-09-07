# src/bin/pg_upgrade_replica/nls.mk
CATALOG_NAME     = pg_upgrade_replica
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   fetch.c \
                   forge_manifest.c \
                   manifest.c \
                   pg_upgrade_replica.c \
                   reuse.c \
                   subprocess.c \
                   ../../common/controldata_utils.c \
                   ../../fe_utils/connect_utils.c \
                   ../../fe_utils/option_utils.c \
                   ../../fe_utils/recovery_gen.c \
                   ../../fe_utils/write_manifest.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS)
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
