#include "commit.h"
#include "tree.h"
#include "index.h"
#include "pes.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// FIX: correct signatures from object.c
extern int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
extern int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out);

// ─── HEAD READ ─────────────────────────────
int head_read(ObjectID *out) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) return -1;

    char line[512];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    line[strcspn(line, "\n")] = 0;

    char path[512];
    if (strncmp(line, "ref: ", 5) == 0) {
        snprintf(path, sizeof(path), "%s/%s", PES_DIR, line + 5);

        f = fopen(path, "r");
        if (!f) return -1;

        if (!fgets(line, sizeof(line), f)) {
            fclose(f);
            return -1;
        }
        fclose(f);
    }

    line[strcspn(line, "\n")] = 0;
    return hex_to_hash(line, out);
}

// ─── HEAD UPDATE ───────────────────────────
int head_update(const ObjectID *id) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) return -1;

    char line[512];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    line[strcspn(line, "\n")] = 0;

    char path[512];
    if (strncmp(line, "ref: ", 5) == 0) {
        snprintf(path, sizeof(path), "%s/%s", PES_DIR, line + 5);
    } else {
        snprintf(path, sizeof(path), "%s", HEAD_FILE);
    }

    char tmp[520];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *out = fopen(tmp, "w");
    if (!out) return -1;

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    fprintf(out, "%s\n", hex);

    fflush(out);
    fsync(fileno(out));
    fclose(out);

    return rename(tmp, path);
}

// ─── COMMIT CREATE ─────────────────────────
int commit_create(const char *message, ObjectID *commit_id_out) {
    ObjectID tree_id;
    if (tree_from_index(&tree_id) != 0)
        return -1;

    Commit c;
    memset(&c, 0, sizeof(c));

    c.tree = tree_id;

    ObjectID parent;
    if (head_read(&parent) == 0) {
        c.parent = parent;
        c.has_parent = 1;
    } else {
        c.has_parent = 0;
    }

    snprintf(c.author, sizeof(c.author), "%s", pes_author());
    c.timestamp = (uint64_t)time(NULL);
    snprintf(c.message, sizeof(c.message), "%s", message);

    void *raw;
    size_t len;

    if (commit_serialize(&c, &raw, &len) != 0)
        return -1;

    ObjectID cid;
    if (object_write(OBJ_COMMIT, raw, len, &cid) != 0) {
        free(raw);
        return -1;
    }

    free(raw);

    if (head_update(&cid) != 0)
        return -1;

    if (commit_id_out)
        *commit_id_out = cid;

    return 0;
}

// ─── FIXED commit_walk (THIS WAS YOUR ERROR) ───────────────────────────────
int commit_walk(commit_walk_fn callback, void *ctx) {
    ObjectID id;
    if (head_read(&id) != 0) return -1;

    while (1) {
        ObjectType type;
        void *raw;
        size_t len;

        if (object_read(&id, &type, &raw, &len) != 0)
            return -1;

        Commit c;
        commit_parse(raw, len, &c);
        free(raw);

        callback(&id, &c, ctx);

        if (!c.has_parent)
            break;

        id = c.parent;
    }

    return 0;
}
int commit_serialize(const Commit *c, void **data_out, size_t *len_out) {
    char tree_hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&c->tree, tree_hex);

    char parent_hex[HASH_HEX_SIZE + 1] = {0};
    if (c->has_parent)
        hash_to_hex(&c->parent, parent_hex);

    char buf[8192];
    int n = 0;

    n += snprintf(buf + n, sizeof(buf) - n, "tree %s\n", tree_hex);

    if (c->has_parent)
        n += snprintf(buf + n, sizeof(buf) - n, "parent %s\n", parent_hex);

    n += snprintf(buf + n, sizeof(buf) - n,
                  "author %s %llu\n"
                  "committer %s %llu\n\n"
                  "%s",
                  c->author,
                  (unsigned long long)c->timestamp,
                  c->author,
                  (unsigned long long)c->timestamp,
                  c->message);

    *data_out = malloc(n + 1);
    memcpy(*data_out, buf, n + 1);
    *len_out = n;

    return 0;
}

int commit_parse(const void *data, size_t len, Commit *out) {
    const char *p = data;
    char hex[65];

    if (sscanf(p, "tree %64s", hex) != 1) return -1;
    hex_to_hash(hex, &out->tree);
    p = strchr(p, '\n') + 1;

    if (strncmp(p, "parent ", 7) == 0) {
        sscanf(p, "parent %64s", hex);
        hex_to_hash(hex, &out->parent);
        out->has_parent = 1;
        p = strchr(p, '\n') + 1;
    } else {
        out->has_parent = 0;
    }

    char author_buf[256];
    sscanf(p, "author %255[^\n]", author_buf);
    char *last = strrchr(author_buf, ' ');
    *last = 0;
    strcpy(out->author, author_buf);
    p = strchr(p, '\n') + 1;

    p = strchr(p, '\n') + 1; // skip committer
    p = strchr(p, '\n') + 1; // blank line

    strncpy(out->message, p, sizeof(out->message));

    return 0;
}
