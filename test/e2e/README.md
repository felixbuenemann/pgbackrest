# myBackRest end-to-end test harness

Two complementary drivers, both running the same `standalone-e2e-test`
binary which connects to a server (TCP or socket) and drives the hot
backup orchestrator end-to-end. The harnesses catch integration bugs
the scripted `harnessMysql` shim can't — real SQL parsing, real lock
semantics, real cross-flavor wire-protocol drift, version-conditional
quirks (SHOW MASTER STATUS rename in MySQL 8.4, MariaDB full_crc32
checksum trailer, undo tablespace naming, etc.).

## run-docker.sh — multi-flavor / multi-version matrix

```
test/e2e/run-docker.sh <flavor> <version>
```

`flavor` is `mysql` | `mariadb` | `percona`. `version` is any tag the
upstream image publishes:

| Flavor   | Image                              | Tested versions    |
|----------|------------------------------------|--------------------|
| mysql    | mysql:VERSION                      | 8.0, 8.4           |
| mariadb  | mariadb:VERSION                    | 10.11, 11.4 (¹)    |
| percona  | percona/percona-server:VERSION     | 8.0                |

¹ MariaDB has a known limitation — see "MariaDB prepared-XA caveat"
below.

The harness does a full round-trip per invocation:

1. Bootstraps a fresh datadir under `/tmp/mybackrest-e2e-docker-<tag>`
2. Starts a container with the datadir bind-mounted at `/var/lib/mysql`
   and the database port exposed on `127.0.0.1:13307` (or override
   with `MYBACKREST_E2E_PORT`)
3. Creates a backup user (privileges branched per flavor: MariaDB
   doesn't recognize `BACKUP_ADMIN`) and a test schema covering
   InnoDB, MyISAM, CSV, and ARCHIVE engines
4. Snapshots the data
5. Runs `standalone-e2e-test` which drives `mysqlHotBackup` via TCP
6. Stops container A
7. Runs `standalone-e2e-restore-test` to lay out a fresh restored
   datadir
8. Starts container B against the restored datadir; InnoDB runs crash
   recovery from the redo log we copied
9. Snapshots data from container B; diffs against step 4

Behavior knobs:

```sh
# Keep work dir for inspection on failure
MYBACKREST_E2E_KEEP=1 test/e2e/run-docker.sh mariadb 11.4

# Override the TCP port (container A uses PORT, container B uses PORT+1)
MYBACKREST_E2E_PORT=23307 test/e2e/run-docker.sh mysql 8.4

# Accept the MariaDB prepared-XA limitation (see below) and pass the
# test as long as the backup half succeeds
MYBACKREST_E2E_ACCEPT_PREPARED_XA=1 test/e2e/run-docker.sh mariadb 11.4
```

Prereqs:

- Docker (or compatible — OrbStack on macOS works) with a running
  daemon
- A `mariadb` or `mysql` client on PATH for the bootstrap (the harness
  doesn't `docker exec` into the server for the schema setup)

## run-mysql.sh — local-mysqld harness (legacy)

```
test/e2e/run-mysql.sh
```

Drives a private mysqld via socket against a `/tmp` datadir without
touching the system `mysql.service`. Predates the Docker harness; the
Docker variant is now the primary integration story. Kept because it
runs ~3× faster than the Docker variant on a single flavor (no image
pull, no container churn).

Pick by what you have available:

- Just want a fast smoke against whatever's apt-installed → `run-mysql.sh`
- Want multi-flavor / multi-version coverage → `run-docker.sh`

## MariaDB prepared-XA caveat

On MariaDB the orchestrator can capture an on-disk state where InnoDB
has an in-flight prepared XA transaction (typically from an internal
binlog rotation that the bootstrap triggers). The restored server then
refuses to start with "Found N prepared transactions! ... start with
--tc-heuristic-recover ...".

`mariabackup` avoids this by waiting at `BACKUP STAGE BLOCK_COMMIT`
for in-flight 2PC operations to complete before snapshotting; the
analog for our orchestrator's lock protocol is open work.

For now, set `MYBACKREST_E2E_ACCEPT_PREPARED_XA=1` to mark the
restore-verify stage as "skipped due to known limitation" and pass on
the backup half. MySQL / Percona aren't affected — they bookend the
2PC inside the standard recovery path.

## Test binary

`standalone-e2e-test` is environment-agnostic:

```sh
MYBACKREST_E2E_HOST=...      # OR MYBACKREST_E2E_SOCKET=...
MYBACKREST_E2E_PORT=...
MYBACKREST_E2E_DATADIR=...   # host path the orchestrator reads from
MYBACKREST_E2E_BACKUP=...    # host path the backup writes into
MYBACKREST_E2E_USER=...
MYBACKREST_E2E_PASS=...
build/src/standalone-e2e-test
```

Vendor- and version-conditional assertions inside the binary handle
cross-flavor differences (lock method autodetect, `auto.cnf` presence,
SHOW BINARY LOG STATUS vs SHOW MASTER STATUS, etc.). Adding a new
flavor to the matrix doesn't require recompiling.
