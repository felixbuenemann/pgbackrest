#!/bin/bash
# ============================================================================
# myBackRest end-to-end test driver — single-flavor / single-version MariaDB
#
# Boots a private mariadbd on a non-default port + datadir under /tmp (does
# NOT touch /var/lib/mysql or the system mariadb.service), runs the e2e test
# binary against it, then shuts everything down and cleans up.
#
# This is a TEMPORARY harness. The eventual Docker-driven Phase H harness
# will replace this script with one that spawns containers from a version
# matrix (mysql:5.7/8.0/8.4, mariadb:10.4/10.5/10.11/11.4, percona:5.7/8.0/8.4)
# but the test BINARY (mariadbE2eTest) is environment-agnostic — it just
# connects to a socket and runs the orchestrator. Swapping the bootstrap
# layer doesn't change the test logic.
# ============================================================================
set -e -u -o pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
WORK_DIR="/tmp/mybackrest-e2e-mariadb"
DATADIR="${WORK_DIR}/datadir"
SOCKET="${WORK_DIR}/mysqld.sock"
PIDFILE="${WORK_DIR}/mysqld.pid"
ERRLOG="${WORK_DIR}/mysqld.err"
PORT="${MYBACKREST_E2E_PORT:-13306}"
TEST_USER="backup"
TEST_PASS="backup_pass"

log() { echo "[e2e] $*" >&2; }

cleanup() {
    local rc=$?
    if [[ -f "$PIDFILE" ]]; then
        local pid
        pid=$(cat "$PIDFILE" 2>/dev/null || true)
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            log "shutting down mariadbd (pid=$pid)"
            kill -TERM "$pid" 2>/dev/null || true
            local i=0
            while kill -0 "$pid" 2>/dev/null && (( i < 30 )); do sleep 1; i=$((i+1)); done
            kill -KILL "$pid" 2>/dev/null || true
        fi
    fi
    if [[ "${MYBACKREST_E2E_KEEP:-0}" != "1" ]]; then
        log "cleaning up $WORK_DIR"
        rm -rf "$WORK_DIR"
    else
        log "MYBACKREST_E2E_KEEP=1 — leaving $WORK_DIR for inspection"
    fi
    exit $rc
}
trap cleanup EXIT INT TERM

# ----------------------------------------------------------------------------
# 1. Prerequisites
# ----------------------------------------------------------------------------
for bin in mariadbd mariadb mariadb-install-db; do
    if ! command -v "$bin" >/dev/null 2>&1; then
        log "missing: $bin (install mariadb-server first)"
        exit 1
    fi
done

if [[ ! -x "${BUILD_DIR}/src/standalone-mariadb-e2e-test" ]]; then
    log "missing test binary at ${BUILD_DIR}/src/standalone-mariadb-e2e-test — build with: meson compile -C build"
    exit 1
fi

# ----------------------------------------------------------------------------
# 2. Fresh datadir
# ----------------------------------------------------------------------------
log "preparing fresh datadir at $DATADIR"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
chmod 700 "$WORK_DIR"

mariadb-install-db \
    --no-defaults \
    --user="$(id -un)" \
    --datadir="$DATADIR" \
    --skip-test-db \
    --auth-root-authentication-method=normal \
    >/dev/null 2>&1

# ----------------------------------------------------------------------------
# 3. Start a private mariadbd
# ----------------------------------------------------------------------------
log "starting private mariadbd on port=$PORT socket=$SOCKET"
mariadbd \
    --no-defaults \
    --datadir="$DATADIR" \
    --socket="$SOCKET" \
    --port="$PORT" \
    --pid-file="$PIDFILE" \
    --log-error="$ERRLOG" \
    --bind-address=127.0.0.1 \
    --user="$(id -un)" \
    --log-bin="$WORK_DIR/binlog" \
    --binlog-format=ROW \
    --server-id=42 \
    --gtid-strict-mode=1 \
    --innodb-buffer-pool-size=64M \
    --innodb-log-file-size=16M \
    --skip-name-resolve \
    &

# Wait for the socket to appear
log "waiting for socket..."
for i in $(seq 1 30); do
    if [[ -S "$SOCKET" ]]; then break; fi
    sleep 1
done
if [[ ! -S "$SOCKET" ]]; then
    log "mariadbd did not produce socket within 30s — error log:"
    cat "$ERRLOG" >&2 || true
    exit 1
fi
log "mariadbd is up"

# ----------------------------------------------------------------------------
# 4. Create a backup user + a sample database
# ----------------------------------------------------------------------------
log "bootstrapping test schema + backup user"
mariadb --socket="$SOCKET" --user=root <<SQL
CREATE USER '$TEST_USER'@'localhost' IDENTIFIED BY '$TEST_PASS';
-- MariaDB-compatible grant set. BACKUP STAGE requires RELOAD; SHOW MASTER STATUS requires REPLICATION CLIENT (or SUPER).
GRANT RELOAD, LOCK TABLES, REPLICATION CLIENT, REPLICATION SLAVE, PROCESS ON *.* TO '$TEST_USER'@'localhost';
GRANT SELECT, SHOW VIEW ON *.* TO '$TEST_USER'@'localhost';
FLUSH PRIVILEGES;

CREATE DATABASE testdb;
USE testdb;
CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64)) ENGINE=InnoDB;
INSERT INTO users VALUES (1, 'alice'), (2, 'bob'), (3, 'carol');
CREATE TABLE legacy (id INT PRIMARY KEY, payload VARCHAR(64)) ENGINE=MyISAM;
INSERT INTO legacy VALUES (10, 'aaa'), (20, 'bbb');
FLUSH BINARY LOGS;
SQL

# ----------------------------------------------------------------------------
# 5. Run the test binary
# ----------------------------------------------------------------------------
BACKUP_DIR="${WORK_DIR}/backup"
mkdir -p "$BACKUP_DIR"

log "running mariadbE2eTest against socket=$SOCKET, datadir=$DATADIR, backup=$BACKUP_DIR"

MYBACKREST_E2E_SOCKET="$SOCKET" \
MYBACKREST_E2E_DATADIR="$DATADIR" \
MYBACKREST_E2E_BACKUP="$BACKUP_DIR" \
MYBACKREST_E2E_USER="$TEST_USER" \
MYBACKREST_E2E_PASS="$TEST_PASS" \
    "${BUILD_DIR}/src/standalone-mariadb-e2e-test"

log "e2e test passed"
