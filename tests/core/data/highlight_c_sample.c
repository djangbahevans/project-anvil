#include <stdio.h>
#include "local.h"
#define MAX_SIZE 100
#define SQUARE(x) ((x) * (x))
#ifdef DEBUG
#endif

typedef struct point { int x; unsigned long y; } point_t;
enum color { RED, GREEN = 2 };

static const char *names[] = {"a\n", 'b'};

/* block
   comment */
int main(int argc, char **argv) {
    // line comment
    for (int i = 0; i < MAX_SIZE; i++) {
        if (argv[i] == NULL) continue;
        printf("%d café 😀 %s\n", i, argv[i]);
    }
    point_t p = {.x = 1};
    p.x += sizeof(int) + 0x1F + 1.5e3;
done:
    return 0;
}
