#!/bin/bash
# ============================================================================
# myBackRest end-to-end test driver — single-flavor / single-version MySQL
#
# Mirror of run-mariadb.sh for MySQL 8.0+. Boots a private mysqld on a
# non-default port + datadir under /tmp (does NOT touch /var/lib/mysql or
# the system mysql.service), runs the e2e test binary against it, then
# shuts down and cleans up.
#
# Exercises the MySQL-specific code paths the MariaDB harness doesn't:
#   - LOCK INSTANCE FOR BACKUP (8.0.16+) as the autodetected lock method
#   - SELECT @@server_uuid (MySQL-only system variable)
#   - auto.cnf auto-generation at first start
#   - SHOW MASTER STATUS with the Executed_Gtid_Set column populated
# ============================================================================
set -e -u -o pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
WORK_DIR="/tmp/mybackrest-e2e-mysql"
DATADIR="${WORK_DIR}/datadir"
SOCKET="${WORK_DIR}/mysqld.sock"
PIDFILE="${WORK_DIR}/mysqld.pid"
ERRLOG="${WORK_DIR}/mysqld.err"
PORT="${MYBACKREST_E2E_PORT:-13307}"
TEST_USER="backup"
TEST_PASS="backup_pass"

log() { echo "[e2e-mysql] $*" >&2; }

cleanup() {
    local rc=$?
    if [[ -f "$PIDFILE" ]]; then
        local pid
        pid=$(cat "$PIDFILE" 2>/dev/null || true)
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            log "shutting down mysqld (pid=$pid)"
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

for bin in mysqld mysql; do
    if ! command -v "$bin" >/dev/null 2>&1; then
        log "missing: $bin (install mysql-server first)"
        exit 1
    fi
done

if [[ ! -x "${BUILD_DIR}/src/standalone-e2e-test" ]]; then
    log "missing test binary at ${BUILD_DIR}/src/standalone-e2e-test — build with: meson compile -C build"
    exit 1
fi

log "preparing fresh datadir at $DATADIR"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
chmod 700 "$WORK_DIR"

# MySQL 8.0 uses mysqld --initialize-insecure (no equivalent to mariadb-install-db).
# --initialize-insecure creates the system tables + an empty-password root user.
mysqld \
    --no-defaults \
    --initialize-insecure \
    --datadir="$DATADIR" \
    --user="$(id -un)" \
    >/dev/null 2>&1

log "starting private mysqld on port=$PORT socket=$SOCKET"
mkdir -p "$WORK_DIR/files"

mysqld \
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
    --gtid-mode=ON \
    --enforce-gtid-consistency=ON \
    --innodb-buffer-pool-size=64M \
    --innodb-redo-log-capacity=16M \
    --skip-name-resolve \
    --mysqlx=OFF \
    --secure-file-priv="$WORK_DIR/files" \
    &

log "waiting for socket..."
for i in $(seq 1 30); do
    if [[ -S "$SOCKET" ]]; then break; fi
    sleep 1
done
if [[ ! -S "$SOCKET" ]]; then
    log "mysqld did not produce socket within 30s — error log:"
    cat "$ERRLOG" >&2 || true
    exit 1
fi
log "mysqld is up"

log "bootstrapping test schema + backup user"
mysql --socket="$SOCKET" --user=root <<SQL
-- MySQL 8.0 BACKUP_ADMIN is required for LOCK INSTANCE FOR BACKUP; mysql_native_password is the simplest auth so libmariadb
-- can connect with a plain password.
CREATE USER '$TEST_USER'@'localhost' IDENTIFIED WITH mysql_native_password BY '$TEST_PASS';
GRANT BACKUP_ADMIN, RELOAD, REPLICATION CLIENT, REPLICATION SLAVE, PROCESS, LOCK TABLES, SELECT ON *.* TO '$TEST_USER'@'localhost';
FLUSH PRIVILEGES;

CREATE DATABASE testdb;
USE testdb;
CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64)) ENGINE=InnoDB;
INSERT INTO users VALUES (1, 'alice'), (2, 'bob'), (3, 'carol');
CREATE TABLE legacy (id INT PRIMARY KEY, payload VARCHAR(64)) ENGINE=MyISAM;
INSERT INTO legacy VALUES (10, 'aaa'), (20, 'bbb');
FLUSH BINARY LOGS;
SQL

BACKUP_DIR="${WORK_DIR}/backup"
mkdir -p "$BACKUP_DIR"

log "running e2e test against socket=$SOCKET, datadir=$DATADIR, backup=$BACKUP_DIR"

MYBACKREST_E2E_SOCKET="$SOCKET" \
MYBACKREST_E2E_DATADIR="$DATADIR" \
MYBACKREST_E2E_BACKUP="$BACKUP_DIR" \
MYBACKREST_E2E_USER="$TEST_USER" \
MYBACKREST_E2E_PASS="$TEST_PASS" \
    "${BUILD_DIR}/src/standalone-e2e-test"

log "e2e test passed (MySQL)"
