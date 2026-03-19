/* Check bit-reversal relationships between DCS codewords */
#include <stdio.h>
#include <stdint.h>
#include "../libplcode/src/plcode_internal.h"

static void pbits(uint32_t v, int n) {
    for (int i = n-1; i >= 0; i--) putchar((v>>i)&1?'1':'0');
}

int main(void) {
    plcode_tables_init();

    printf("=== Bit-reverse mapping for all 104 DCS codes ===\n\n");

    int mapped = 0;
    for (int i = 0; i < PLCODE_DCS_NUM_CODES; i++) {
        uint32_t cw = plcode_dcs_codewords[i];
        uint32_t rev = 0;
        for (int b = 0; b < 23; b++)
            if (cw & (1u<<b)) rev |= (1u<<(22-b));

        for (int j = 0; j < PLCODE_DCS_NUM_CODES; j++) {
            if (plcode_dcs_codewords[j] == rev) {
                int li = plcode_dcs_code_to_label(plcode_dcs_codes[i]);
                int lj = plcode_dcs_code_to_label(plcode_dcs_codes[j]);
                if (li != lj) {
                    printf("  DCS %03d <-> DCS %03d (bit-reversed codewords)\n", li, lj);
                    mapped++;
                }
                break;
            }
        }
    }
    printf("\n%d mappings found\n", mapped);

    /* Specific checks */
    printf("\n=== Specific codes ===\n");
    int check[] = {25, 26, 244, 464};
    for (int c = 0; c < 4; c++) {
        int idx = plcode_dcs_code_index(check[c]);
        uint32_t cw = plcode_dcs_codewords[idx];
        printf("DCS %03d: ", check[c]);
        pbits(cw, 23);
        printf("\n");
    }

    return 0;
}
