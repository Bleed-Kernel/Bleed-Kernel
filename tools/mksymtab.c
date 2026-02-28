#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/// @brief kernel symbol table
struct ksym {
    uint64_t addr;
    uint32_t name_off;
};

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <nm.txt> <out>\n", argv[0]);
        return 1;
    }

    FILE *in = fopen(argv[1], "r");
    if (!in) {
        perror("fopen input");
        return 1;
    }

    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        perror("fopen output");
        fclose(in);
        return 1;
    }

    struct ksym *syms = NULL;
    size_t sc = 0, syms_cap = 0;

    char *strtab = NULL;
    size_t so = 1, strtab_cap = 0;

    strtab_cap = 4096;
    strtab = malloc(strtab_cap);
    if (!strtab) {
        perror("malloc strtab");
        fclose(in);
        fclose(out);
        return 1;
    }
    strtab[0] = '\0';

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;

    while ((line_len = getline(&line, &line_cap, in)) > 0) {
        (void)line_len;
        uint64_t addr = 0;
        char type = 0;
        char name[512] = {0};

        if (sscanf(line, "%" SCNx64 " %c %511s", &addr, &type, name) != 3)
            continue;

        if (type != 'T' && type != 't')
            continue;

        size_t name_len = strlen(name) + 1;
        if (name_len > UINT32_MAX) {
            fprintf(stderr, "symbol name too long\n");
            free(line);
            free(strtab);
            free(syms);
            fclose(in);
            fclose(out);
            return 1;
        }

        while (so + name_len > strtab_cap) {
            size_t new_cap = strtab_cap * 2;
            char *tmp = realloc(strtab, new_cap);
            if (!tmp) {
                perror("realloc strtab");
                free(line);
                free(strtab);
                free(syms);
                fclose(in);
                fclose(out);
                return 1;
            }
            strtab = tmp;
            strtab_cap = new_cap;
        }

        if (sc == syms_cap) {
            size_t new_cap = syms_cap ? syms_cap * 2 : 4096;
            struct ksym *tmp = realloc(syms, new_cap * sizeof(*syms));
            if (!tmp) {
                perror("realloc syms");
                free(line);
                free(strtab);
                free(syms);
                fclose(in);
                fclose(out);
                return 1;
            }
            syms = tmp;
            syms_cap = new_cap;
        }

        syms[sc].addr = addr;
        syms[sc].name_off = (uint32_t)so;
        memcpy(&strtab[so], name, name_len);
        so += name_len;
        sc++;
    }

    if (fwrite(&sc, sizeof(sc), 1, out) != 1 ||
        (sc && fwrite(syms, sizeof(struct ksym), sc, out) != sc) ||
        fwrite(strtab, 1, so, out) != so) {
        perror("fwrite");
        free(line);
        free(strtab);
        free(syms);
        fclose(in);
        fclose(out);
        return 1;
    }

    free(line);
    free(strtab);
    free(syms);
    fclose(in);
    fclose(out);
    return 0;
}
