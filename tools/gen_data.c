#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s [row_count] [output_path]\n", argv0);
    fprintf(stderr, "  row_count defaults to 200000\n");
    fprintf(stderr, "  output_path defaults to data/users.dat\n");
}

static long parse_rows(const char *text) {
    char *end = NULL;
    long rows = strtol(text, &end, 10);

    if (!end || *end != '\0' || rows < 0)
        return -1;
    return rows;
}

int main(int argc, char **argv) {
    long rows = 200000;
    const char *path = "data/users.dat";

    if (argc > 3) {
        usage(argv[0]);
        return 1;
    }

    if (argc >= 2) {
        rows = parse_rows(argv[1]);
        if (rows < 0) {
            usage(argv[0]);
            return 1;
        }
    }

    if (argc >= 3)
        path = argv[2];

    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "gen_data: cannot open '%s': %s\n",
                path, strerror(errno));
        return 1;
    }

    for (long i = 1; i <= rows; i++) {
        long age = 20 + (i % 50);
        fprintf(fp, "%ld | user%ld | %ld | user%ld@example.com\n",
                i, i, age, i);
    }

    if (fclose(fp) != 0) {
        fprintf(stderr, "gen_data: failed to close '%s': %s\n",
                path, strerror(errno));
        return 1;
    }

    printf("generated %ld rows into %s\n", rows, path);
    return 0;
}
