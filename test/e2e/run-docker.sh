#!/usr/bin/env bash
# ============================================================================
# myBackRest end-to-end test driver — Docker matrix (MySQL / MariaDB / Percona)
#
# Usage: run-docker.sh <flavor> <version>
#   flavor:  mysql | mariadb | percona
#   version: image tag, e.g. 8.4, 8.0, 5.7, 10.11, 11.4
#
# Examples:
#   test/e2e/run-docker.sh mysql 8.4
#   test/e2e/run-docker.sh mariadb 10.11
#   test/e2e/run-docker.sh percona 8.0
#
# Stages (mirrors run-mysql.sh's flow, but with containers):
#   1. Start container A with the source datadir bind-mounted
#   2. Bootstrap the test schema via the official client (mysql/mariadb)
#   3. Snapshot the data
#   4. standalone-e2e-test connects via TCP, runs the hot backup
#   5. Stop container A (datadir survives via the bind mount)
#   6. standalone-e2e-restore-test restores the backup into a fresh host dir
#   7. Start container B with the restored datadir bind-mounted; InnoDB's
#      automatic recovery replays the redo log we copied
#   8. Snapshot data from container B; diff against step 3
#   9. Stop container B + cleanup
# ============================================================================
set -e -u -o pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 <mysql|mariadb|percona> <version>" >&2
    exit 1
fi

FLAVOR="$1"
VERSION="$2"

case "$FLAVOR" in
    mysql)   IMAGE="mysql:${VERSION}"                 ;;
    mariadb) IMAGE="mariadb:${VERSION}"               ;;
    percona) IMAGE="percona/percona-server:${VERSION}";;
    *) echo "unknown flavor: $FLAVOR (mysql/mariadb/percona)" >&2; exit 1 ;;
esac

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
TAG="${FLAVOR}-${VERSION//./}"                       # mysql84, mariadb1011, percona80
WORK_DIR="/tmp/mybackrest-e2e-docker-${TAG}"
DATADIR_A="${WORK_DIR}/datadir-source"
DATADIR_B="${WORK_DIR}/datadir-restored"
BACKUP_DIR="${WORK_DIR}/backup"
SNAPSHOT_BEFORE="${WORK_DIR}/snapshot-before.txt"
SNAPSHOT_AFTER="${WORK_DIR}/snapshot-after.txt"
PORT="${MYBACKREST_E2E_PORT:-13307}"
NAME_A="mybackrest-e2e-${TAG}-A"
NAME_B="mybackrest-e2e-${TAG}-B"
TEST_USER="backup"
TEST_PASS="backup_pass"

# Pick a client binary on the host — the mariadb client speaks both wire protocols, so we can use whatever the host has.
MYSQL_CLIENT="$(command -v mariadb || command -v mysql || true)"
if [[ -z "$MYSQL_CLIENT" ]]; then
    echo "no mysql / mariadb client on host PATH — install one (brew install mysql-client or mariadb)" >&2
    exit 1
fi

log() { echo "[e2e-docker:${FLAVOR}-${VERSION}] $*" >&2; }

stop_container() {
    local name="$1"
    if docker ps -q -f name="^${name}\$" 2>/dev/null | grep -q .; then
        log "stopping container $name"
        docker stop "$name" >/dev/null 2>&1 || true
    fi
    docker rm -f "$name" >/dev/null 2>&1 || true
}

cleanup() {
    local rc=$?
    stop_container "$NAME_A"
    stop_container "$NAME_B"
    if [[ "${MYBACKREST_E2E_KEEP:-0}" != "1" ]]; then
        log "cleaning up $WORK_DIR"
        rm -rf "$WORK_DIR" 2>/dev/null || sudo rm -rf "$WORK_DIR" 2>/dev/null || true
    else
        log "MYBACKREST_E2E_KEEP=1 — leaving $WORK_DIR for inspection"
    fi
    exit $rc
}
trap cleanup EXIT INT TERM

# Timeouts (seconds). 30s is the universal cap — anything slower than that is a hang, not legitimate work. Tune via env
# only if a particularly slow CI box needs more headroom.
WAIT_FOR_READY=${MYBACKREST_E2E_WAIT_READY:-30}              # container A or B becoming reachable on its port
TIMEOUT_DOCKER_OP=${MYBACKREST_E2E_TIMEOUT_DOCKER:-30}        # one-off `docker run` (XA pre-pass etc.)
TIMEOUT_MYSQL_CLIENT=${MYBACKREST_E2E_TIMEOUT_CLIENT:-30}     # mysql client command (bootstrap, snapshot)
TIMEOUT_TEST_BIN=${MYBACKREST_E2E_TIMEOUT_TEST:-30}           # standalone-e2e-* binaries

# Convenience wrapper: kill a command if it runs past the deadline so we don't sit on a hang.
withTimeout() {
    local secs="$1"; shift
    timeout "${secs}s" "$@"
}

wait_for_mysql() {
    local name="$1" port="$2" pass="$3"
    local label="$4"
    log "waiting for $label on 127.0.0.1:${port} (timeout ${WAIT_FOR_READY}s)"
    for i in $(seq 1 "$WAIT_FOR_READY"); do
        # If the container exited prematurely the logs explain why; surface them immediately rather than wait the deadline
        if ! docker ps -q -f name="^${name}\$" 2>/dev/null | grep -q .; then
            log "$label container is no longer running after ${i}s; recent logs:"
            docker logs "$name" 2>&1 | tail -40 >&2 || true
            return 1
        fi
        # 3s client timeout — each poll is its own probe, we don't want to wait long if the server's slow to respond yet
        if withTimeout 3 "$MYSQL_CLIENT" --protocol=tcp --host=127.0.0.1 --port="$port" \
              --user=root --password="$pass" -e 'SELECT 1' >/dev/null 2>&1; then
            log "$label ready after ${i}s"
            return 0
        fi
        sleep 1
    done
    log "$label did not become ready within ${WAIT_FOR_READY}s; container logs:"
    docker logs "$name" 2>&1 | tail -40 >&2 || true
    return 1
}

# ----------------------------------------------------------------------------
# Prereqs
# ----------------------------------------------------------------------------
if ! docker ps >/dev/null 2>&1; then
    log "docker daemon not reachable"
    exit 1
fi

for bin in \
    "${BUILD_DIR}/src/standalone-e2e-test" \
    "${BUILD_DIR}/src/standalone-e2e-restore-test"; do
    [[ -x "$bin" ]] || { log "missing $bin — run: meson compile -C build"; exit 1; }
done

# ----------------------------------------------------------------------------
# Stage 1: fresh datadir + container A
# ----------------------------------------------------------------------------
log "preparing $WORK_DIR"
rm -rf "$WORK_DIR" 2>/dev/null || sudo rm -rf "$WORK_DIR" 2>/dev/null || true
mkdir -p "$DATADIR_A" "$BACKUP_DIR"
chmod 777 "$WORK_DIR" "$DATADIR_A" "$BACKUP_DIR"

# Flavor-specific server flags
COMMON_ARGS=(
    --log-bin
    --binlog-format=ROW
    --server-id=42
)

case "$FLAVOR" in
    mysql|percona)
        SERVER_ARGS=("${COMMON_ARGS[@]}" --gtid-mode=ON --enforce-gtid-consistency=ON --mysqlx=OFF)
        # MySQL 8.4+ ships mysql_native_password as a disabled-by-default plugin. We use it because libmariadb's TLS-less
        # caching_sha2_password path requires either an RSA pre-fetched key or a secure connection — both add setup we
        # don't need here. Enable the legacy plugin on 8.4+ so the existing CREATE USER ... IDENTIFIED WITH
        # mysql_native_password works.
        if [[ "${VERSION%%.*}" -ge 8 ]] && [[ "${VERSION#*.}" != "0" ]]; then
            # 8.1, 8.2, 8.3, 8.4 — anything 8.x that isn't 8.0 needs the flag
            if [[ "${VERSION#*.}" != "0" ]]; then
                SERVER_ARGS+=(--mysql-native-password=ON)
            fi
        fi
        ;;
    mariadb)
        SERVER_ARGS=("${COMMON_ARGS[@]}")           # MariaDB defaults are sufficient
        ;;
esac

log "starting container $NAME_A from image $IMAGE"
docker run -d \
    --name "$NAME_A" \
    -e MYSQL_ROOT_PASSWORD=root \
    -e MARIADB_ROOT_PASSWORD=root \
    -v "$DATADIR_A":/var/lib/mysql \
    -p "127.0.0.1:${PORT}:3306" \
    "$IMAGE" "${SERVER_ARGS[@]}" \
    >/dev/null

wait_for_mysql "$NAME_A" "$PORT" "root" "container A"

# ----------------------------------------------------------------------------
# Stage 2: bootstrap schema + backup user
# ----------------------------------------------------------------------------
log "bootstrapping test schema + backup user"

if [[ "$FLAVOR" == "mariadb" ]]; then
    # MariaDB doesn't recognize BACKUP_ADMIN
    GRANTS="GRANT RELOAD, LOCK TABLES, REPLICATION CLIENT, REPLICATION SLAVE, PROCESS, SELECT ON *.* TO '${TEST_USER}'@'%';"
    CREATE_USER="CREATE USER '${TEST_USER}'@'%' IDENTIFIED BY '${TEST_PASS}';"
else
    # MySQL 8.0+ / Percona 8.0+ — use BACKUP_ADMIN + native password auth so libmariadb can connect with a plain password
    GRANTS="GRANT BACKUP_ADMIN, RELOAD, REPLICATION CLIENT, REPLICATION SLAVE, PROCESS, LOCK TABLES, SELECT ON *.* TO '${TEST_USER}'@'%';"
    CREATE_USER="CREATE USER '${TEST_USER}'@'%' IDENTIFIED WITH mysql_native_password BY '${TEST_PASS}';"
fi

withTimeout "$TIMEOUT_MYSQL_CLIENT" "$MYSQL_CLIENT" --protocol=tcp --host=127.0.0.1 --port="$PORT" --user=root --password=root <<SQL
$CREATE_USER
$GRANTS
FLUSH PRIVILEGES;

CREATE DATABASE testdb;
USE testdb;
CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64)) ENGINE=InnoDB;
INSERT INTO users VALUES (1, 'alice'), (2, 'bob'), (3, 'carol');
CREATE TABLE legacy (id INT PRIMARY KEY, payload VARCHAR(64)) ENGINE=MyISAM;
INSERT INTO legacy VALUES (10, 'aaa'), (20, 'bbb');
CREATE TABLE log_csv (id INT NOT NULL, msg VARCHAR(64) NOT NULL) ENGINE=CSV;
INSERT INTO log_csv VALUES (1, 'event-one'), (2, 'event-two');
SQL

# ARCHIVE is bundled-and-loaded by default on MySQL/Percona but ships as an opt-in plugin on modern MariaDB (10.6+). Try
# to load it; if it succeeds, include an ARCHIVE table in the schema, otherwise skip ARCHIVE for this run. The orchestrator
# still gets tested by every other engine (InnoDB + MyISAM + CSV).
HAS_ARCHIVE=false
if withTimeout "$TIMEOUT_MYSQL_CLIENT" "$MYSQL_CLIENT" --protocol=tcp --host=127.0.0.1 --port="$PORT" --user=root --password=root \
    -e "INSTALL SONAME 'ha_archive'" >/dev/null 2>&1 || \
   withTimeout "$TIMEOUT_MYSQL_CLIENT" "$MYSQL_CLIENT" --protocol=tcp --host=127.0.0.1 --port="$PORT" --user=root --password=root \
    -e "SELECT 1 FROM INFORMATION_SCHEMA.ENGINES WHERE ENGINE='ARCHIVE' AND SUPPORT IN ('YES','DEFAULT')" 2>&1 | grep -q "^1\$"
then
    HAS_ARCHIVE=true
fi

if [[ "$HAS_ARCHIVE" == "true" ]]; then
    withTimeout "$TIMEOUT_MYSQL_CLIENT" "$MYSQL_CLIENT" --protocol=tcp --host=127.0.0.1 --port="$PORT" --user=root --password=root <<SQL
USE testdb;
CREATE TABLE audit (id INT PRIMARY KEY AUTO_INCREMENT, evt VARCHAR(64)) ENGINE=ARCHIVE;
INSERT INTO audit (evt) VALUES ('login'), ('logout'), ('login');
SQL
    log "ARCHIVE engine loaded — included in test schema"
else
    log "ARCHIVE engine not available — testing without it"
fi

# Note: we deliberately do NOT call FLUSH BINARY LOGS here. On MariaDB the binlog rotation can leave an in-flight XA
# transaction in the InnoDB redo at the moment we acquire BACKUP STAGE, which the restored server then refuses to start
# without --tc-heuristic-recover. mariabackup waits for the rotation to fully flush; the analog for our orchestrator is a
# future enhancement. For the e2e we just don't trigger a rotation right before the backup.

# ----------------------------------------------------------------------------
# Stage 3: snapshot source data
# ----------------------------------------------------------------------------
log "snapshotting source data"
withTimeout "$TIMEOUT_MYSQL_CLIENT" "$MYSQL_CLIENT" --protocol=tcp --host=127.0.0.1 --port="$PORT" --user=root --password=root \
    --batch --raw --skip-column-names \
    -e "SELECT id, name FROM testdb.users ORDER BY id; SELECT '---';
        SELECT id, payload FROM testdb.legacy ORDER BY id; SELECT '---';
        SELECT id, msg FROM testdb.log_csv ORDER BY id; SELECT '---';
        SELECT id, evt FROM testdb.audit ORDER BY id" \
    > "$SNAPSHOT_BEFORE"

# ----------------------------------------------------------------------------
# Stage 4: run the e2e backup test (orchestrator via TCP, reads bind-mounted datadir)
# ----------------------------------------------------------------------------
log "running e2e backup test"
MYBACKREST_E2E_HOST=127.0.0.1 \
MYBACKREST_E2E_PORT="$PORT" \
MYBACKREST_E2E_DATADIR="$DATADIR_A" \
MYBACKREST_E2E_BACKUP="$BACKUP_DIR" \
MYBACKREST_E2E_USER="$TEST_USER" \
MYBACKREST_E2E_PASS="$TEST_PASS" \
    withTimeout "$TIMEOUT_TEST_BIN" "${BUILD_DIR}/src/standalone-e2e-test"
log "backup test passed"

# ----------------------------------------------------------------------------
# Stage 5: stop container A
# ----------------------------------------------------------------------------
stop_container "$NAME_A"

# ----------------------------------------------------------------------------
# Stage 6: restore into fresh datadir
# ----------------------------------------------------------------------------
log "running e2e restore test"
mkdir -p "$DATADIR_B"
chmod 777 "$DATADIR_B"

MYBACKREST_E2E_BACKUP="$BACKUP_DIR" \
MYBACKREST_E2E_RESTORE="$DATADIR_B" \
    withTimeout "$TIMEOUT_TEST_BIN" "${BUILD_DIR}/src/standalone-e2e-restore-test"
log "restore test passed"

# ----------------------------------------------------------------------------
# Stage 7: start container B against the restored datadir
# ----------------------------------------------------------------------------
log "starting container $NAME_B against restored datadir (InnoDB recovery expected)"
# Server args for the recovery startup:
#   - Keep binlog ON so MariaDB can use the binlog as transaction coordinator when resolving prepared XA transactions.
#     Without it MariaDB 10.x refuses to start with "Found N prepared transactions!".
#   - --tc-heuristic-recover=COMMIT instructs the server to commit any prepared transactions it can't safely resolve
#     (matches the xtrabackup / mariabackup convention: a backup is taken under a write-blocking lock, so any prepared
#     transactions captured in the backup were going to commit anyway).
#   - --skip-grant-tables would conflict with bootstrap done by entrypoint, so we leave it off.
B_ARGS=()
if [[ "$FLAVOR" == "mysql" || "$FLAVOR" == "percona" ]]; then
    B_ARGS+=(--mysqlx=OFF)
    if [[ "${VERSION%%.*}" -ge 8 ]] && [[ "${VERSION#*.}" != "0" ]]; then
        B_ARGS+=(--mysql-native-password=ON)
    fi
fi

start_container_B() {
    local extra_args=("$@")
    # ${arr[@]+"${arr[@]}"} idiom = "expand only if non-empty" — needed because `set -u` errors on empty array expansion.
    docker run -d \
        --name "$NAME_B" \
        -e MYSQL_ROOT_PASSWORD=root \
        -e MARIADB_ROOT_PASSWORD=root \
        -v "$DATADIR_B":/var/lib/mysql \
        -p "127.0.0.1:$((PORT + 1)):3306" \
        "$IMAGE" ${B_ARGS[@]+"${B_ARGS[@]}"} ${extra_args[@]+"${extra_args[@]}"} \
        >/dev/null
}

start_container_B
if ! wait_for_mysql "$NAME_B" "$((PORT + 1))" "root" "container B"; then
    # Capture the failure logs BEFORE any cleanup so we can scrutinize. wait_for_mysql already streamed them to stderr;
    # capture again for the conditional check.
    B_LOGS=$(docker logs "$NAME_B" 2>&1 || true)
    log "container B is in 'stopped' state. Captured $(echo "$B_LOGS" | wc -l | tr -d ' ') log line(s)"

    # MariaDB-specific: when the orchestrator captured an in-flight 2PC and we don't have the original binlog files
    # (Phase F archive-push not yet implemented), mariadbd refuses to start with "Found N prepared transactions". The
    # restore-side fix is a one-shot mariadbd run with --tc-heuristic-recover=COMMIT which resolves the XAs and exits.
    # Our prepareWriteRecoveryFiles writes mybackrest_recovery_xa.cnf in the restore dir for exactly this; the harness
    # uses it to drive the pre-pass via docker.
    if [[ "$FLAVOR" == "mariadb" ]] && echo "$B_LOGS" | grep -q "prepared transaction"; then
        log "container B saw prepared XA transactions — running one-shot XA pre-pass via docker"
        docker rm -f "$NAME_B" >/dev/null 2>&1 || true

        # Run mariadbd directly with --tc-heuristic-recover=COMMIT against the bind-mounted restore datadir. The
        # tc-heuristic-recover flag is one-shot — mariadbd commits prepared XAs then exits with "Can't init tc log"
        # error code 1, which is expected. We don't use --rm because OrbStack/macOS bind-mount sync can lag if the
        # container exits AND is reaped in the same instant; using `docker run` foreground then explicit `docker rm`
        # gives the bind-mount writes time to flush.
        XA_RECOVER_NAME="${NAME_B}-xa-recover"
        # Cap the recovery pass at TIMEOUT_DOCKER_OP — it should finish in 5-15s. If it hangs longer, kill it and let the
        # subsequent normal start surface the underlying error.
        withTimeout "$TIMEOUT_DOCKER_OP" docker run --name "$XA_RECOVER_NAME" \
            -e MARIADB_ROOT_PASSWORD=root \
            -v "$DATADIR_B":/var/lib/mysql \
            "$IMAGE" --tc-heuristic-recover=COMMIT >/dev/null 2>&1 || true
        sleep 2
        docker rm -f "$XA_RECOVER_NAME" >/dev/null 2>&1 || true

        log "XA pre-pass complete — retrying container B normal start"
        start_container_B
        wait_for_mysql "$NAME_B" "$((PORT + 1))" "root" "container B (post-XA-pre-pass)"
    else
        exit 1
    fi
fi
log "container B is up — InnoDB recovery completed"

# ----------------------------------------------------------------------------
# Stage 8: snapshot + compare
# ----------------------------------------------------------------------------
log "snapshotting restored data"
"$MYSQL_CLIENT" --protocol=tcp --host=127.0.0.1 --port="$((PORT + 1))" --user=root --password=root \
    --batch --raw --skip-column-names \
    -e "SELECT id, name FROM testdb.users ORDER BY id; SELECT '---';
        SELECT id, payload FROM testdb.legacy ORDER BY id; SELECT '---';
        SELECT id, msg FROM testdb.log_csv ORDER BY id; SELECT '---';
        SELECT id, evt FROM testdb.audit ORDER BY id" \
    > "$SNAPSHOT_AFTER"

log "comparing snapshots"
if diff -u "$SNAPSHOT_BEFORE" "$SNAPSHOT_AFTER"; then
    log "PASS: restored data matches source byte-for-byte ($FLAVOR-$VERSION)"
else
    log "FAIL: restored data differs from source"
    exit 1
fi

stop_container "$NAME_B"
log "full round-trip passed: $FLAVOR-$VERSION"
