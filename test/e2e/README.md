# myBackRest end-to-end test harness

Temporary single-flavor / single-version e2e harness that runs the hot
backup orchestrator against a real, locally-installed MariaDB server.
Catches integration bugs that the scripted `harnessMysql` shim can't —
real SQL parsing, real lock semantics, real SHOW MASTER STATUS column
ordering, vendor-specific quirks like the absence of `@@server_uuid`
on MariaDB.

## What it does

1. Installs `mariadb-server` via apt-get (one-time setup; not redone on
   subsequent runs).
2. Builds a fresh private datadir under `/tmp/mybackrest-e2e-mariadb/`.
3. Starts a private `mariadbd` on a non-default port (13306) with its
   own socket — does NOT touch `/var/lib/mysql` or the system
   `mariadb.service`.
4. Creates a `backup` user with `RELOAD, LOCK TABLES, REPLICATION
   CLIENT, REPLICATION SLAVE, PROCESS` (the MariaDB-compatible
   privilege set for a hot backup).
5. Populates a small test schema with InnoDB and MyISAM tables.
6. Runs `standalone-mariadb-e2e-test` which connects via `MysqlClient`
   and drives `mysqlHotBackup` end-to-end.
7. Verifies the backup directory's structure, file presence, and
   manifest round-trip.
8. Stops `mariadbd` and tears down the work directory.

## Running

```sh
# One-time setup:
sudo apt-get install -y mariadb-server libmariadb-dev

# Build the test binary:
meson setup build && meson compile -C build

# Run the harness:
test/e2e/run-mariadb.sh
```

To keep the datadir + backup directory for inspection after a failure:

```sh
MYBACKREST_E2E_KEEP=1 test/e2e/run-mariadb.sh
```

To pick a different port:

```sh
MYBACKREST_E2E_PORT=23306 test/e2e/run-mariadb.sh
```

## Why this exists

The full multi-flavor / multi-version test matrix needs Docker (so we
can spawn `mysql:5.7`, `mysql:8.0`, `mysql:8.4`, `mariadb:10.4`, ...,
`percona:5.7`, etc. in parallel). That harness is Phase H of the
project plan and is blocked on developing in a Docker-capable
environment.

In the meantime, this single-instance harness gives us a real
end-to-end signal — every change to the hot backup orchestrator can
be sanity-checked against a real server before landing.

## Swapping to Docker later

The test binary `standalone-mariadb-e2e-test` is environment-
agnostic — it just connects to a socket and runs the orchestrator. To
graduate to a Docker matrix, only this script needs to change. The
replacement would:

1. For each (flavor, version) tuple in the matrix:
   - `docker run --name mybackrest-e2e-${flavor}-${version} \
        -v /tmp/.../datadir:/var/lib/mysql \
        -p 13306:3306 \
        ${flavor}:${version}`
   - Wait for the container's healthcheck
   - Run the same SQL bootstrap (CREATE USER, CREATE TABLE)
   - Run `standalone-mariadb-e2e-test` (renamed appropriately) with
     `MYBACKREST_E2E_SOCKET` swapped for `MYBACKREST_E2E_HOST` +
     `MYBACKREST_E2E_PORT` if TCP is preferred
   - `docker rm -f` on exit

2. The test binary stays unchanged; the result struct + assertions
   apply to every flavor. Vendor-conditional assertions (e.g.
   `auto.cnf` only on MySQL/Percona, BACKUP STAGE only on MariaDB
   10.4+) already exist in the test source.

## Known limitations

- Single flavor (MariaDB) and single version (whatever
  apt-get installs on the host). Catches some quirks; misses
  cross-vendor wire-protocol drift.
- Test machine state pollution: `mariadb-server` is installed and
  may auto-start a system instance. The harness uses `--no-defaults`
  so it doesn't clobber the system config.
- No restore-side coverage in this iteration. The cold restore
  orchestrator is unit-tested via `standalone-cold-restore-test`;
  wiring up a full backup → restore → mysqld --recover loop in the
  e2e harness is a follow-up.
