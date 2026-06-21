// Host unit test for the firmware password code (soniclear_pwd.c).
// Build & run: make -C .. test     (or: cc -I.. ../soniclear_pwd.c test_pwd.c -o t && ./t)
#include "soniclear_pwd.h"
#include <stdio.h>
#include <string.h>

struct vec {
    const char* name;
    uint8_t uid[7];
    const char* mfg;
    uint8_t pwd[4];
};

// Synthetic regression vector (not a real head) - kept in sync with
// soniclear.py --selftest. Guards the CRC16 chain + byte-swap.
static const struct vec vectors[] = {
    {"synthetic",
     {0x04, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06},
     "010203 99Z",
     {0x80, 0xB0, 0x22, 0xC1}},
};

int main(void) {
    int fails = 0;
    for(size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        const struct vec* v = &vectors[i];
        uint8_t out[4];
        soniclear_pwd_compute(v->uid, (const uint8_t*)v->mfg, strlen(v->mfg), out);
        int ok = memcmp(out, v->pwd, 4) == 0;
        printf(
            "%-10s got %02X%02X%02X%02X  %s\n",
            v->name,
            out[0],
            out[1],
            out[2],
            out[3],
            ok ? "OK" : "FAIL");
        fails += !ok;
    }
    if(fails) {
        printf("%d test(s) FAILED\n", fails);
        return 1;
    }
    printf("all password tests passed\n");
    return 0;
}
