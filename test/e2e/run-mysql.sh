#!/bin/bash
# ============================================================================
# myBackRest end-to-end test driver — full backup → restore → re-run loop
#
# Stages:
#   1. Boot a private mysqld_A on a /tmp datadir; populate a test schema
#   2. Snapshot the data (mysqldump-style SELECT)
#   3. standalone-e2e-test: hot backup of mysqld_A
#   4. Shut down mysqld_A
#   5. standalone-e2e-restore-test: restore the backup into a fresh datadir
#   6. Boot mysqld_B against the restored datadir (relies on InnoDB's automatic
#      crash recovery to replay the redo log we copied)
#   7. Snapshot data from mysqld_B; assert it matches step 2
#   8. Shut down mysqld_B, clean up
#
# Does NOT touch /var/lib/mysql or the system mysql.service. Both private
# servers run with --no-defaults on private sockets/ports.
# ============================================================================
set -e -u -o pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
WORK_DIR="/tmp/mybackrest-e2e-mysql"
DATADIR_A="${WORK_DIR}/datadir-source"
DATADIR_B="${WORK_DIR}/datadir-restored"
BACKUP_DIR="${WORK_DIR}/backup"
SOCKET_A="${WORK_DIR}/mysqld-A.sock"
SOCKET_B="${WORK_DIR}/mysqld-B.sock"
PIDFILE_A="${WORK_DIR}/mysqld-A.pid"
PIDFILE_B="${WORK_DIR}/mysqld-B.pid"
ERRLOG_A="${WORK_DIR}/mysqld-A.err"
ERRLOG_B="${WORK_DIR}/mysqld-B.err"
SNAPSHOT_BEFORE="${WORK_DIR}/snapshot-before.txt"
SNAPSHOT_AFTER="${WORK_DIR}/snapshot-after.txt"
PORT_A="${MYBACKREST_E2E_PORT:-13307}"
PORT_B=$((PORT_A + 1))
TEST_USER="backup"
TEST_PASS="backup_pass"

log() { echo "[e2e-mysql] $*" >&2; }

# Cleanly stop a private mysqld given its pidfile
stop_mysqld() {
    local pidfile="$1"
    if [[ -f "$pidfile" ]]; then
        local pid
        pid=$(cat "$pidfile" 2>/dev/null || true)
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
            local i=0
            while kill -0 "$pid" 2>/dev/null && (( i < 30 )); do sleep 1; i=$((i+1)); done
            kill -KILL "$pid" 2>/dev/null || true
        fi
    fi
}

cleanup() {
    local rc=$?
    stop_mysqld "$PIDFILE_A"
    stop_mysqld "$PIDFILE_B"
    if [[ "${MYBACKREST_E2E_KEEP:-0}" != "1" ]]; then
        log "cleaning up $WORK_DIR"
        rm -rf "$WORK_DIR"
    else
        log "MYBACKREST_E2E_KEEP=1 — leaving $WORK_DIR for inspection"
    fi
    exit $rc
}
trap cleanup EXIT INT TERM

# Wait until a socket exists or fail with the errorlog dumped
wait_for_socket() {
    local socket="$1" errlog="$2" label="$3"
    log "waiting for $label socket..."
    for i in $(seq 1 30); do
        [[ -S "$socket" ]] && return 0
        sleep 1
    done
    log "$label did not produce socket within 30s — error log:"
    cat "$errlog" >&2 || true
    return 1
}

# ----------------------------------------------------------------------------
# Prereqs
# ----------------------------------------------------------------------------
for bin in mysqld mysql; do
    if ! command -v "$bin" >/dev/null 2>&1; then
        log "missing: $bin (install mysql-server first)"
        exit 1
    fi
done

if [[ ! -x "${BUILD_DIR}/src/standalone-e2e-test" ]] || \
   [[ ! -x "${BUILD_DIR}/src/standalone-e2e-restore-test" ]]; then
    log "missing test binaries — build with: meson compile -C build"
    exit 1
fi

# ----------------------------------------------------------------------------
# Stage 1: bootstrap source datadir + start mysqld_A
# ----------------------------------------------------------------------------
log "preparing fresh datadir at $DATADIR_A"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/files"
chmod 700 "$WORK_DIR"

mysqld \
    --no-defaults \
    --initialize-insecure \
    --datadir="$DATADIR_A" \
    --user="$(id -un)" \
    >/dev/null 2>&1

log "starting mysqld_A on port=$PORT_A socket=$SOCKET_A"
mysqld \
    --no-defaults \
    --datadir="$DATADIR_A" \
    --socket="$SOCKET_A" \
    --port="$PORT_A" \
    --pid-file="$PIDFILE_A" \
    --log-error="$ERRLOG_A" \
    --bind-address=127.0.0.1 \
    --user="$(id -un)" \
    --log-bin="$WORK_DIR/binlog-A" \
    --binlog-format=ROW \
    --server-id=42 \
    --gtid-mode=ON \
    --enforce-gtid-consistency=ON \
    --innodb-buffer-pool-size=64M \
    --innodb-redo-log-capacity=16M \
    --skip-name-resolve \
    --mysqlx=OFF \
    --secure-file-priv="$WORK_DIR/files" \
    &

wait_for_socket "$SOCKET_A" "$ERRLOG_A" "mysqld_A"
log "mysqld_A is up"

# ----------------------------------------------------------------------------
# Stage 2: bootstrap test schema + snapshot data
# ----------------------------------------------------------------------------
log "bootstrapping test schema + backup user"
mysql --socket="$SOCKET_A" --user=root <<SQL
CREATE USER '$TEST_USER'@'localhost' IDENTIFIED WITH mysql_native_password BY '$TEST_PASS';
GRANT BACKUP_ADMIN, RELOAD, REPLICATION CLIENT, REPLICATION SLAVE, PROCESS, LOCK TABLES, SELECT ON *.* TO '$TEST_USER'@'localhost';
FLUSH PRIVILEGES;

CREATE DATABASE testdb;
USE testdb;
CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64)) ENGINE=InnoDB;
INSERT INTO users VALUES (1, 'alice'), (2, 'bob'), (3, 'carol');
CREATE TABLE legacy (id INT PRIMARY KEY, payload VARCHAR(64)) ENGINE=MyISAM;
INSERT INTO legacy VALUES (10, 'aaa'), (20, 'bbb');
-- Exercise the CSV + ARCHIVE engine handlers too
CREATE TABLE log_csv (id INT NOT NULL, msg VARCHAR(64) NOT NULL) ENGINE=CSV;
INSERT INTO log_csv VALUES (1, 'event-one'), (2, 'event-two');
CREATE TABLE audit (id INT PRIMARY KEY AUTO_INCREMENT, evt VARCHAR(64)) ENGINE=ARCHIVE;
INSERT INTO audit (evt) VALUES ('login'), ('logout'), ('login');
FLUSH BINARY LOGS;
SQL

log "snapshotting source data"
mysql --socket="$SOCKET_A" --user=root --batch --raw --skip-column-names \
    -e "SELECT id, name FROM testdb.users ORDER BY id; SELECT '---';
        SELECT id, payload FROM testdb.legacy ORDER BY id; SELECT '---';
        SELECT id, msg FROM testdb.log_csv ORDER BY id; SELECT '---';
        SELECT id, evt FROM testdb.audit ORDER BY id" \
    > "$SNAPSHOT_BEFORE"

# ----------------------------------------------------------------------------
# Stage 3: hot backup
# ----------------------------------------------------------------------------
mkdir -p "$BACKUP_DIR"

log "running e2e backup test"
MYBACKREST_E2E_SOCKET="$SOCKET_A" \
MYBACKREST_E2E_DATADIR="$DATADIR_A" \
MYBACKREST_E2E_BACKUP="$BACKUP_DIR" \
MYBACKREST_E2E_USER="$TEST_USER" \
MYBACKREST_E2E_PASS="$TEST_PASS" \
    "${BUILD_DIR}/src/standalone-e2e-test"
log "backup test passed"

# ----------------------------------------------------------------------------
# Stage 4: shut down source
# ----------------------------------------------------------------------------
log "shutting down mysqld_A"
stop_mysqld "$PIDFILE_A"

# ----------------------------------------------------------------------------
# Stage 5: restore into fresh datadir
# ----------------------------------------------------------------------------
log "running e2e restore test"
mkdir -p "$DATADIR_B"
chmod 700 "$DATADIR_B"

MYBACKREST_E2E_BACKUP="$BACKUP_DIR" \
MYBACKREST_E2E_RESTORE="$DATADIR_B" \
    "${BUILD_DIR}/src/standalone-e2e-restore-test"
log "restore test passed"

# ----------------------------------------------------------------------------
# Stage 6: start mysqld_B on restored datadir
# ----------------------------------------------------------------------------
log "starting mysqld_B on port=$PORT_B socket=$SOCKET_B (recovery expected)"
mysqld \
    --no-defaults \
    --datadir="$DATADIR_B" \
    --socket="$SOCKET_B" \
    --port="$PORT_B" \
    --pid-file="$PIDFILE_B" \
    --log-error="$ERRLOG_B" \
    --bind-address=127.0.0.1 \
    --user="$(id -un)" \
    --skip-log-bin \
    --innodb-buffer-pool-size=64M \
    --skip-name-resolve \
    --mysqlx=OFF \
    --secure-file-priv="$WORK_DIR/files" \
    &

wait_for_socket "$SOCKET_B" "$ERRLOG_B" "mysqld_B"
log "mysqld_B is up — InnoDB completed recovery successfully"

# ----------------------------------------------------------------------------
# Stage 7: snapshot data from restored server + compare
# ----------------------------------------------------------------------------
log "snapshotting restored data"
mysql --socket="$SOCKET_B" --user=root --batch --raw --skip-column-names \
    -e "SELECT id, name FROM testdb.users ORDER BY id; SELECT '---';
        SELECT id, payload FROM testdb.legacy ORDER BY id; SELECT '---';
        SELECT id, msg FROM testdb.log_csv ORDER BY id; SELECT '---';
        SELECT id, evt FROM testdb.audit ORDER BY id" \
    > "$SNAPSHOT_AFTER"

log "comparing snapshots"
if diff -u "$SNAPSHOT_BEFORE" "$SNAPSHOT_AFTER"; then
    log "PASS: restored data matches source byte-for-byte"
else
    log "FAIL: restored data differs from source"
    exit 1
fi

# ----------------------------------------------------------------------------
# Stage 8: clean shutdown
# ----------------------------------------------------------------------------
log "shutting down mysqld_B"
stop_mysqld "$PIDFILE_B"

log "full round-trip passed: backup → restore → re-run with intact data"
