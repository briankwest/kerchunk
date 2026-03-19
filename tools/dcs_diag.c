/*
 * dcs_diag.c — DCS decoder diagnostic.
 * Prints raw shift register, bit order, and codeword matching details.
 *
 * Tests: encode DCS 025, decode, and show what the decoder sees at each step.
 *
 * Build:
 *   cc -O2 -Ilibplcode/include -o dcs_diag tools/dcs_diag.c libplcode/libplcode.a -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../libplcode/src/plcode_internal.h"

static void print_bits(uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--)
        putchar((v >> i) & 1 ? '1' : '0');
}

int main(void)
{
    plcode_tables_init();

    printf("=== DCS 025 Codeword Analysis ===\n\n");

    /* What's the internal binary for code 025? */
    uint16_t code_label = 25;  /* DCS label 025 */
    int idx = plcode_dcs_code_index(code_label);
    printf("Code label: %03d\n", code_label);
    printf("Table index: %d\n", idx);
    if (idx >= 0) {
        printf("Internal binary (plcode_dcs_codes[%d]): 0%03o = 0x%03x = ",
               idx, plcode_dcs_codes[idx], plcode_dcs_codes[idx]);
        print_bits(plcode_dcs_codes[idx], 9);
        printf("\n");
    }

    /* What codeword does the encoder produce? */
    uint32_t cw = plcode_dcs_codewords[idx];
    printf("\nGolay codeword: 0x%06x = ", cw);
    print_bits(cw, 23);
    printf("\n");

    /* Break down the codeword */
    uint16_t data12 = (uint16_t)(cw >> 11);
    uint16_t parity11 = (uint16_t)(cw & 0x7FF);
    printf("  Data (bits 22:11):   0x%03x = ", data12);
    print_bits(data12, 12);
    printf("\n");
    printf("  Parity (bits 10:0):  0x%03x = ", parity11);
    print_bits(parity11, 11);
    printf("\n");

    /* Check marker bit position */
    printf("  data12 & 0xE00 = 0x%03x (expect 0x200 for TIA/EIA-603)\n",
           data12 & 0xE00);
    printf("  code9 = data12 & 0x1FF = 0x%03x = 0%03o (expect 025)\n",
           data12 & 0x1FF, data12 & 0x1FF);

    printf("\n=== Encoder Bit Stream ===\n");
    printf("Encoder sends LSB first (bit_index 0, 1, 2, ... 22):\n  ");

    /* Simulate encoder — print bits in transmission order */
    for (int b = 0; b < 23; b++) {
        int bit = (cw >> b) & 1;
        printf("%d", bit);
        if (b == 8) printf("|");  /* code/marker boundary */
        if (b == 11) printf("|"); /* data/parity boundary */
    }
    printf("\n  ");
    printf("code(9)  |mrk|parity(11)\n");

    printf("\n=== Decoder Shift Register Simulation ===\n");
    printf("Decoder: right-shift, insert at bit 22\n\n");

    uint32_t sr = 0;
    for (int b = 0; b < 23; b++) {
        int bit = (cw >> b) & 1;  /* encoder sends LSB first */
        sr = ((sr >> 1) | ((uint32_t)bit << 22)) & 0x7FFFFF;
    }

    printf("After 23 bits, shift_reg = 0x%06x = ", sr);
    print_bits(sr, 23);
    printf("\n");
    printf("Original codeword      = 0x%06x = ", cw);
    print_bits(cw, 23);
    printf("\n");
    printf("Match: %s\n", sr == cw ? "YES" : "NO");

    if (sr != cw) {
        /* Check if it's bit-reversed */
        uint32_t rev = 0;
        for (int i = 0; i < 23; i++)
            if (cw & (1u << i)) rev |= (1u << (22 - i));
        printf("Bit-reversed codeword  = 0x%06x = ", rev);
        print_bits(rev, 23);
        printf("\nReversed match: %s\n", sr == rev ? "YES" : "NO");
    }

    /* Try extracting code from the shift register as the decoder would */
    printf("\n=== Decoder Extract (from shift_reg) ===\n");
    data12 = (uint16_t)(sr >> 11);
    printf("data12 = sr >> 11 = 0x%03x = ", data12);
    print_bits(data12, 12);
    printf("\n");
    printf("  data12 & 0xE00 = 0x%03x\n", data12 & 0xE00);
    printf("  code9 = 0x%03x = 0%03o = label %d\n",
           data12 & 0x1FF, data12 & 0x1FF,
           plcode_dcs_code_to_label(data12 & 0x1FF));

    /* Also try extracting from bits 0:11 (in case data is at LSB end) */
    printf("\n=== Alternative: data from low bits ===\n");
    data12 = (uint16_t)(sr & 0xFFF);
    printf("data12 = sr & 0xFFF = 0x%03x = ", data12);
    print_bits(data12, 12);
    printf("\n");
    printf("  data12 & 0xE00 = 0x%03x\n", data12 & 0xE00);
    printf("  code9 = 0x%03x = 0%03o = label %d\n",
           data12 & 0x1FF, data12 & 0x1FF,
           plcode_dcs_code_to_label(data12 & 0x1FF));

    /* Show all 104 codes and their codewords for reference */
    printf("\n=== Code 025 vs Code 411 ===\n");
    int idx411 = plcode_dcs_code_index(411);
    if (idx411 >= 0) {
        printf("Code 411 codeword: 0x%06x = ", plcode_dcs_codewords[idx411]);
        print_bits(plcode_dcs_codewords[idx411], 23);
        printf("\n");
    }

    printf("\nCode 025 codeword: 0x%06x = ", plcode_dcs_codewords[idx]);
    print_bits(plcode_dcs_codewords[idx], 23);
    printf("\n");

    return 0;
}
