# myBackRest end-to-end test harness

Temporary single-flavor / single-version e2e harness that runs the hot
backup orchestrator against a real, locally-installed MySQL server.
Catches integration bugs that the scripted `harnessMysql` shim can't —
real SQL parsing, real lock semantics, real SHOW MASTER STATUS column
ordering, vendor-specific quirks like `auto.cnf` auto-generation.

## Why MySQL (and not MariaDB or both)?

This is the bridge until a Docker-driven multi-flavor / multi-version
matrix lands (Phase H of the project plan). One flavor is enough to
catch the integration-level bugs that the scripted shim misses; running
both against a single locally-installed package would just complicate
the harness without adding meaningful coverage on a one-machine setup
(apt-installed `mysql-server` and `mariadb-server` conflict — they
both provide `/usr/sbin/mysqld` and can't coexist).

MySQL is the picked flavor because the orchestrator's MySQL paths are
the more common production target (LOCK INSTANCE FOR BACKUP, real
`auto.cnf`, Executed_Gtid_Set populated in SHOW MASTER STATUS).
MariaDB's BACKUP STAGE state machine + `@@gtid_binlog_pos` paths are
covered by `harnessMysql`-driven unit tests; they'll get real-server
coverage once the Docker matrix arrives.

The test binary itself (`standalone-e2e-test`) is vendor-agnostic.
Vendor- and version-conditional assertions already exist for each
orchestrator behavior that differs across flavors. Pointing the
existing binary at a different server flavor just requires a new
driver script.

## What it does

1. Builds a fresh private datadir under `/tmp/mybackrest-e2e-mysql/`.
2. Initializes it via `mysqld --initialize-insecure`.
3. Starts a private `mysqld` on a non-default port (13307) with its
   own socket — does NOT touch `/var/lib/mysql` or the system
   `mysql.service`.
4. Creates a `backup` user with `BACKUP_ADMIN, RELOAD, REPLICATION
   CLIENT, REPLICATION SLAVE, PROCESS, LOCK TABLES, SELECT` (the
   MySQL 8.0+ privilege set for a hot backup with LOCK INSTANCE FOR
   BACKUP).
5. Populates a test schema with InnoDB and MyISAM tables.
6. Runs `standalone-e2e-test` which connects via `MysqlClient` and
   drives `mysqlHotBackup` end-to-end.
7. Verifies the backup directory's structure, file presence, and
   manifest round-trip.
8. Stops `mysqld` and tears down the work directory.

## Running

```sh
# One-time setup:
sudo apt-get install -y mysql-server libmariadb-dev

# Build the test binary:
meson setup build && meson compile -C build

# Run the harness:
test/e2e/run-mysql.sh
```

To keep the datadir + backup directory for inspection after a failure:

```sh
MYBACKREST_E2E_KEEP=1 test/e2e/run-mysql.sh
```

To pick a different port:

```sh
MYBACKREST_E2E_PORT=23307 test/e2e/run-mysql.sh
```

## Graduating to Docker

The test binary `standalone-e2e-test` is environment-agnostic — it
just connects to a socket and runs the orchestrator. To swap in a
Docker matrix harness (Phase H):

1. For each (flavor, version) tuple in the matrix
   (`mysql:5.7/8.0/8.4`, `mariadb:10.4/10.5/10.11/11.4`,
   `percona:5.7/8.0/8.4`):
   - `docker run` the image with a mounted datadir
   - Wait for the container's healthcheck
   - Run the same SQL bootstrap appropriate for the flavor (the
     current MySQL bootstrap uses `BACKUP_ADMIN` which doesn't exist
     on MariaDB; the Docker harness will branch the GRANT per flavor)
   - Run `standalone-e2e-test` against the container's socket
   - `docker rm -f` on exit

2. The test binary stays unchanged — vendor- and version-conditional
   assertions inside the binary already handle the cross-flavor
   differences in lock method, `auto.cnf` generation, etc.

## Known limitations

- Single flavor + single version (MySQL 8.0.x via apt). Catches some
  integration bugs; misses cross-vendor wire-protocol drift.
- Backup-only coverage. A future iteration will extend the harness
  with a restore-into-fresh-datadir + start-mysqld-against-it loop to
  end-to-end-verify the cold restore path.
