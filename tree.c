#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pes.h"

// ─── helpers ─────────────────────────────────────────────────────────────────

static int cmp_tree(const void *a, const void *b) {
    return strcmp(((TreeEntry *)a)->name,
                  ((TreeEntry *)b)->name);
}

static int cmp_index_path(const void *a, const void *b) {
    return strcmp(((IndexEntry *)a)->path,
                  ((IndexEntry *)b)->path);
}

// Forward declaration
static int build_tree(IndexEntry *entries, int count,
                      const char *prefix, ObjectID *id_out);

// ─── tree_serialize ───────────────────────────────────────────────────────────

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    char buf[8192];
    int n = 0;

    Tree tmp = *tree;
    qsort(tmp.entries, tmp.count, sizeof(TreeEntry), cmp_tree);

    for (int i = 0; i < tmp.count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&tmp.entries[i].hash, hex);

        n += snprintf(buf + n, sizeof(buf) - n,
                      "%06o %s %s\n",
                      tmp.entries[i].mode,
                      hex,
                      tmp.entries[i].name);
    }

    *data_out = malloc(n + 1);
    memcpy(*data_out, buf, n + 1);
    *len_out = n;
    return 0;
}

// ─── tree_parse ───────────────────────────────────────────────────────────────

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    const char *p = data;
    tree_out->count = 0;

    while ((size_t)(p - (char *)data) < len) {
        TreeEntry e;
        char hex[HASH_HEX_SIZE + 1];

        if (sscanf(p, "%o %64s %255s",
                   &e.mode,
                   hex,
                   e.name) != 3)
            break;

        hex_to_hash(hex, &e.hash);
        tree_out->entries[tree_out->count++] = e;

        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }

    return 0;
}

// ─── build_tree (recursive helper) ───────────────────────────────────────────

static int build_tree(IndexEntry *entries, int count,
                      const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        // Path relative to current directory level
        const char *rel = entries[i].path + strlen(prefix);

        char *slash = strchr(rel, '/');

        if (!slash) {
            // Direct file in this directory — add as blob entry
            TreeEntry e;
            e.mode = entries[i].mode;
            e.hash = entries[i].hash;
            snprintf(e.name, sizeof(e.name), "%s", rel);
            tree.entries[tree.count++] = e;
            i++;
        } else {
            // File is inside a subdirectory
            // Extract the subdirectory name e.g. "src" from "src/main.c"
            int dir_len = slash - rel;
            char dir_name[256];
            snprintf(dir_name, sizeof(dir_name), "%.*s", dir_len, rel);

            // Collect ALL entries that belong to this same subdirectory
            int j = i;
            while (j < count) {
                const char *r = entries[j].path + strlen(prefix);
                if (strncmp(r, dir_name, dir_len) != 0 ||
                    r[dir_len] != '/') break;
                j++;
            }

            // Build new prefix for recursive call e.g. "src/"
            char new_prefix[512];
            snprintf(new_prefix, sizeof(new_prefix),
                     "%s%s/", prefix, dir_name);

            // Recursively build and store the subtree
            ObjectID sub_id;
            build_tree(entries + i, j - i, new_prefix, &sub_id);

            // Add the subtree as a directory entry
            TreeEntry e;
            e.mode = 040000;
            e.hash = sub_id;
            snprintf(e.name, sizeof(e.name), "%s", dir_name);
            tree.entries[tree.count++] = e;

            i = j;
        }
    }

    // Serialize and write this tree to the object store
    void *raw;
    size_t raw_len;
    tree_serialize(&tree, &raw, &raw_len);
    object_write(OBJ_TREE, raw, raw_len, id_out);
    free(raw);
    return 0;
}

// ─── weak stub (only used by test_tree which does not link index.c) ──────────

__attribute__((weak)) int index_load(Index *index) {
    index->count = 0;
    return 0;
}

// ─── tree_from_index ──────────────────────────────────────────────────────────

int tree_from_index(ObjectID *id_out) {
    Index index;
    index_load(&index);

    // Sort entries by path so subdirectory grouping works correctly
    qsort(index.entries, index.count, sizeof(IndexEntry), cmp_index_path);

    return build_tree(index.entries, index.count, "", id_out);
}
