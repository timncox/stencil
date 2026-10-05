/* Driver for web_parity.mjs: reads commands on stdin, answers with the C
 * engine's result, one line each.
 *   M <text>        -> Morse bits as 0/1
 *   P <fit> <rate> <text> -> st_build_pattern in MORSE: "len beats bits"
 *   C <hex bytes>   -> st_apply_cmd(cmd = first byte, payload = rest);
 *                      prints "rc user_len user_mask word0..7 source index"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stencil.h"

int main(void)
{
    char line[4096];
    while (fgets(line, sizeof line, stdin))
    {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') line[--n] = 0;
        if (line[0] == 'M' && line[1] == ' ')
        {
            uint32_t bits[ST_MAX_UNITS / 32] = {0};
            uint32_t len = st_morse_encode(line + 2, bits, ST_MAX_UNITS);
            for (uint32_t i = 0; i < len; i++) putchar(((bits[i >> 5] >> (i & 31)) & 1u) ? '1' : '0');
            putchar('\n');
        }
        else if (line[0] == 'P' && line[1] == ' ')
        {
            char *p = line + 2;
            unsigned fit = (unsigned)strtoul(p, &p, 10);
            unsigned rate = (unsigned)strtoul(p, &p, 10);
            if (*p == ' ') p++;
            st_params_t pr;
            st_store_t s;
            static st_pattern_t pt;
            st_default_params(&pr);
            st_default_store(&s);
            pr.source = ST_SRC_MORSE; pr.word = 0; pr.fit = (uint8_t)fit; pr.rate = (uint8_t)rate;
            strncpy(s.words[0], p, ST_WORD_MAX);
            s.words[0][ST_WORD_MAX] = 0;
            st_build_pattern(&pr, &s, &pt);
            printf("%u %u ", pt.len, pt.beats);
            for (uint32_t i = 0; i < pt.len; i++) putchar(st_pattern_open(&pt, 0, i) ? '1' : '0');
            putchar('\n');
        }
        else if (line[0] == 'C' && line[1] == ' ')
        {
            uint8_t b[512];
            uint32_t k = 0;
            for (char *p = line + 2; *p && k < sizeof b;)
            {
                while (*p == ' ') p++;
                if (!*p) break;
                b[k++] = (uint8_t)strtoul(p, &p, 16);
            }
            st_store_t s;
            st_params_t pr;
            st_default_store(&s);
            st_default_params(&pr);
            int rc = k ? st_apply_cmd(&s, &pr, b[0], b + 1, k - 1) : 9;
            unsigned ix = pr.source == ST_SRC_BANK ? pr.bank : pr.source == ST_SRC_EUCLID ? pr.hits : pr.word;
            printf("%d %u %08x", rc, s.user_len, s.user_mask);
            for (int w = 0; w < (int)ST_WORDS; w++) printf(" [%s]", s.words[w]);
            printf(" %u %u\n", pr.source, ix);
        }
        fflush(stdout);
    }
    return 0;
}
