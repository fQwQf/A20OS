/* Real SQLite Unix VFS acceptance: no custom VFS or no-op locking shim. */
#include "sqlite3.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *path = "/mlibc-sqlite-acceptance.db";
static void sql(sqlite3 *db, const char *statement) {
    char *error = NULL;
    int rc = sqlite3_exec(db, statement, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQLITE_NATIVE: FAIL rc=%d errno=%d sql=%s error=%s\n",
                rc, errno, statement, error ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
        exit(1);
    }
}
static void expect(sqlite3 *db, const char *statement, const char *expected) {
    sqlite3_stmt *query = NULL;
    if (sqlite3_prepare_v2(db, statement, -1, &query, NULL) != SQLITE_OK ||
        sqlite3_step(query) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(query, 0), expected)) {
        fprintf(stderr, "SQLITE_NATIVE: FAIL query=%s expected=%s error=%s\n",
                statement, expected, sqlite3_errmsg(db));
        exit(1);
    }
    sqlite3_finalize(query);
}
static void *writer(void *opaque) {
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK)
        return (void *)1;
    sqlite3_busy_timeout(db, 5000);
    for (unsigned i = 0; i < 32; i++) {
        char query[128];
        snprintf(query, sizeof(query),
                 "INSERT INTO records VALUES(%u, 'thread')",
                 1000 + (unsigned)(uintptr_t)opaque * 32 + i);
        sql(db, query);
    }
    return (void *)(uintptr_t)(sqlite3_close(db) != SQLITE_OK);
}
int main(int argc, char **argv) {
    sqlite3 *db = NULL;
    if (argc == 2 && !strcmp(argv[1], "contended")) {
        int rc = sqlite3_open(path, &db);
        if (rc == SQLITE_OK)
            rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
        sqlite3_close(db);
        return rc == SQLITE_BUSY ? 0 : 2;
    }
    unlink(path);
    unlink("/mlibc-sqlite-acceptance.db-wal");
    unlink("/mlibc-sqlite-acceptance.db-shm");
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "SQLITE_NATIVE: FAIL open errno=%d error=%s\n", errno,
                db ? sqlite3_errmsg(db) : "allocation");
        return 1;
    }
    sqlite3_busy_timeout(db, 5000);
    expect(db, "PRAGMA journal_mode=WAL", "wal");
    sql(db, "PRAGMA synchronous=FULL; CREATE TABLE records(id INTEGER PRIMARY "
            "KEY, data TEXT); BEGIN IMMEDIATE");
    sqlite3_stmt *insert = NULL;
    if (sqlite3_prepare_v2(db, "INSERT INTO records VALUES(?, ?)", -1, &insert,
                           NULL) != SQLITE_OK)
        return 3;
    for (unsigned i = 0; i < 128; i++) {
        char value[128];
        snprintf(value, sizeof(value), "native-row-%u", i);
        sqlite3_bind_int(insert, 1, i);
        sqlite3_bind_text(insert, 2, value, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insert) != SQLITE_DONE) {
            fprintf(stderr, "SQLITE_NATIVE: FAIL insert=%u error=%s\n", i,
                    sqlite3_errmsg(db));
            return 4;
        }
        sqlite3_reset(insert);
    }
    sqlite3_finalize(insert);
    sql(db,
        "COMMIT; BEGIN; INSERT INTO records VALUES(999, 'rollback'); ROLLBACK");
    expect(db, "SELECT count(*) || ':' || sum(id) FROM records", "128:8128");
    sql(db, "BEGIN IMMEDIATE");
    pid_t child = fork();
    if (!child) {
        execl(argv[0], argv[0], "contended", NULL);
        _exit(5);
    }
    int status;
    if (child < 0 || waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "SQLITE_NATIVE: FAIL cross-process exclusion\n");
        return 6;
    }
    sql(db, "COMMIT");
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; i++)
        if (pthread_create(&threads[i], NULL, writer, (void *)(uintptr_t)i))
            return 7;
    for (unsigned i = 0; i < 2; i++) {
        void *value;
        if (pthread_join(threads[i], &value) || value)
            return 8;
    }
    expect(db, "SELECT count(*) FROM records", "192");
    expect(db, "PRAGMA integrity_check", "ok");
    sql(db, "PRAGMA wal_checkpoint(TRUNCATE)");
    if (sqlite3_close(db) != SQLITE_OK || sqlite3_open(path, &db) != SQLITE_OK)
        return 9;
    expect(db, "SELECT count(*) FROM records", "192");
    expect(db, "PRAGMA integrity_check", "ok");
    sqlite3_close(db);
    unlink(path);
    unlink("/mlibc-sqlite-acceptance.db-wal");
    unlink("/mlibc-sqlite-acceptance.db-shm");
    printf("SQLITE_NATIVE: PASS version=%s WAL commit rollback reopen pthreads "
           "process-locks integrity\n",
           sqlite3_libversion());
    return 0;
}
