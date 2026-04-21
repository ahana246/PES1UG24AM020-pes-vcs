// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

static int cmp_index_entries(const void *a, const void *b) {
    const IndexEntry *ea = (const IndexEntry *)a;
    const IndexEntry *eb = (const IndexEntry *)b;
    return strcmp(ea->path, eb->path);
}

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }

            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTATION ──────────────────────────────────────────────────────────

int index_load(Index *index) {
    index->count = 0;

    FILE *fp = fopen(".pes/index", "r");
    if (!fp) return 0;  // doesn't exist yet = empty index

    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry e;
        char hex[HASH_HEX_SIZE + 1];
        unsigned long mtime_tmp;

        int rc = fscanf(fp, "%o %64s %lu %u %255s",
                        &e.mode,
                        hex,
                        &mtime_tmp,
                        &e.size,
                        e.path);

        if (rc == EOF || rc != 5) break;

        e.mtime_sec = (uint64_t)mtime_tmp;

        if (hex_to_hash(hex, &e.hash) != 0) {
            fclose(fp);
            return -1;
        }

        index->entries[index->count++] = e;
    }

    fclose(fp);
    return 0;
}

int index_save(const Index *index) {
    // Heap-allocate sorted copy — Index struct is too large for the stack
    Index *tmp = malloc(sizeof(Index));
    if (!tmp) return -1;
    *tmp = *index;
    qsort(tmp->entries, tmp->count, sizeof(IndexEntry), cmp_index_entries);

    FILE *fp = fopen(".pes/index.tmp", "w");
    if (!fp) { free(tmp); return -1; }

    for (int i = 0; i < tmp->count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&tmp->entries[i].hash, hex);

        if (fprintf(fp, "%06o %s %lu %u %s\n",
                    tmp->entries[i].mode,
                    hex,
                    (unsigned long)tmp->entries[i].mtime_sec,
                    tmp->entries[i].size,
                    tmp->entries[i].path) < 0) {
            fclose(fp);
            unlink(".pes/index.tmp");
            free(tmp);
            return -1;
        }
    }

    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    free(tmp);

    if (rename(".pes/index.tmp", ".pes/index") != 0) {
        unlink(".pes/index.tmp");
        return -1;
    }

    return 0;
}

int index_add(Index *index, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "error: cannot stat '%s'\n", path);
        return -1;
    }

    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "error: '%s' is not a regular file\n", path);
        return -1;
    }

    // Read file contents
    void *buf = NULL;
    size_t file_size = (size_t)st.st_size;

    if (file_size > 0) {
        FILE *fp = fopen(path, "rb");
        if (!fp) return -1;

        buf = malloc(file_size);
        if (!buf) { fclose(fp); return -1; }

        if (fread(buf, 1, file_size, fp) != file_size) {
            free(buf);
            fclose(fp);
            return -1;
        }
        fclose(fp);
    }

    // Write blob to object store
    ObjectID oid;
    if (object_write(OBJ_BLOB, buf, file_size, &oid) != 0) {
        free(buf);
        return -1;
    }
    free(buf);

    // Build index entry
    IndexEntry e;
    e.mode      = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    e.hash      = oid;
    e.mtime_sec = (unsigned long)st.st_mtime;
    e.size      = file_size;
    snprintf(e.path, sizeof(e.path), "%s", path);

    // Update existing entry or append new one
    IndexEntry *existing = index_find(index, path);
    if (existing) {
        *existing = e;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        index->entries[index->count++] = e;
    }

    return index_save(index);
}
