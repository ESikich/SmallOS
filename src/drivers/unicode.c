#include "unicode.h"

void utf8_decoder_reset(utf8_decoder_t* dec) {
    if (!dec) {
        return;
    }

    dec->codepoint = 0;
    dec->min_codepoint = 0;
    dec->remaining = 0;
}

utf8_decode_result_t utf8_decoder_feed(utf8_decoder_t* dec,
                                       unsigned char byte,
                                       unsigned int* out_codepoint) {
    unsigned int cp;

    if (!dec || !out_codepoint) {
        if (out_codepoint) {
            *out_codepoint = UNICODE_REPLACEMENT_CHAR;
        }
        return UTF8_DECODE_REJECT;
    }

    if (dec->remaining > 0) {
        if ((byte & 0xC0u) != 0x80u) {
            utf8_decoder_reset(dec);
            *out_codepoint = UNICODE_REPLACEMENT_CHAR;
            return UTF8_DECODE_REJECT_RETRY;
        }

        dec->codepoint = (dec->codepoint << 6) | (byte & 0x3Fu);
        dec->remaining--;
        if (dec->remaining > 0) {
            return UTF8_DECODE_WAIT;
        }

        cp = dec->codepoint;
        if (cp < dec->min_codepoint ||
            (cp >= 0xD800u && cp <= 0xDFFFu) ||
            cp > 0x10FFFFu) {
            cp = UNICODE_REPLACEMENT_CHAR;
        }
        utf8_decoder_reset(dec);
        *out_codepoint = cp;
        return UTF8_DECODE_ACCEPT;
    }

    if (byte < 0x80u) {
        *out_codepoint = byte;
        return UTF8_DECODE_ACCEPT;
    }

    if (byte >= 0xC2u && byte <= 0xDFu) {
        dec->codepoint = byte & 0x1Fu;
        dec->min_codepoint = 0x80u;
        dec->remaining = 1;
        return UTF8_DECODE_WAIT;
    }

    if (byte >= 0xE0u && byte <= 0xEFu) {
        dec->codepoint = byte & 0x0Fu;
        dec->min_codepoint = 0x800u;
        dec->remaining = 2;
        return UTF8_DECODE_WAIT;
    }

    if (byte >= 0xF0u && byte <= 0xF4u) {
        dec->codepoint = byte & 0x07u;
        dec->min_codepoint = 0x10000u;
        dec->remaining = 3;
        return UTF8_DECODE_WAIT;
    }

    *out_codepoint = UNICODE_REPLACEMENT_CHAR;
    return UTF8_DECODE_REJECT;
}

unsigned char unicode_to_cp437(unsigned int cp) {
    if (cp < 0x80u) return (unsigned char)cp;

    switch (cp) {
        case 0x00A1u: return 0xADu; /* inverted exclamation */
        case 0x00A2u: return 0x9Bu; /* cent */
        case 0x00A3u: return 0x9Cu; /* pound */
        case 0x00A5u: return 0x9Du; /* yen */
        case 0x00AAu: return 0xA6u;
        case 0x00ABu: return 0xAEu;
        case 0x00ACu: return 0xAAu;
        case 0x00B0u: return 0xF8u;
        case 0x00B1u: return 0xF1u;
        case 0x00B2u: return 0xFDu;
        case 0x00B5u: return 0xE6u;
        case 0x00B7u: return 0xFAu;
        case 0x00BAu: return 0xA7u;
        case 0x00BBu: return 0xAFu;
        case 0x00BCu: return 0xACu;
        case 0x00BDu: return 0xABu;
        case 0x00BFu: return 0xA8u;
        case 0x00C4u: return 0x8Eu;
        case 0x00C5u: return 0x8Fu;
        case 0x00C6u: return 0x92u;
        case 0x00C7u: return 0x80u;
        case 0x00C9u: return 0x90u;
        case 0x00D1u: return 0xA5u;
        case 0x00D6u: return 0x99u;
        case 0x00DCu: return 0x9Au;
        case 0x00DFu: return 0xE1u;
        case 0x00E0u: return 0x85u;
        case 0x00E1u: return 0xA0u;
        case 0x00E2u: return 0x83u;
        case 0x00E4u: return 0x84u;
        case 0x00E5u: return 0x86u;
        case 0x00E6u: return 0x91u;
        case 0x00E7u: return 0x87u;
        case 0x00E8u: return 0x8Au;
        case 0x00E9u: return 0x82u;
        case 0x00EAu: return 0x88u;
        case 0x00EBu: return 0x89u;
        case 0x00ECu: return 0x8Du;
        case 0x00EDu: return 0xA1u;
        case 0x00EEu: return 0x8Cu;
        case 0x00EFu: return 0x8Bu;
        case 0x00F1u: return 0xA4u;
        case 0x00F2u: return 0x95u;
        case 0x00F3u: return 0xA2u;
        case 0x00F4u: return 0x93u;
        case 0x00F6u: return 0x94u;
        case 0x00F7u: return 0xF6u;
        case 0x00F9u: return 0x97u;
        case 0x00FAu: return 0xA3u;
        case 0x00FBu: return 0x96u;
        case 0x00FCu: return 0x81u;
        case 0x00FFu: return 0x98u;
        case 0x0192u: return 0x9Fu;
        case 0x203Cu: return 0x13u;
        case 0x20A7u: return 0x9Eu;
        case 0x25ACu: return 0x16u;
        case 0x25B2u: return 0x1Eu;
        case 0x25BAu: return 0x10u;
        case 0x25BCu: return 0x1Fu;
        case 0x25C4u: return 0x11u;
        case 0x263Au: return 0x01u;
        case 0x263Bu: return 0x02u;
        case 0x263Cu: return 0x0Fu;
        case 0x2640u: return 0x0Cu;
        case 0x2642u: return 0x0Bu;
        case 0x2660u: return 0x06u;
        case 0x2663u: return 0x05u;
        case 0x2665u: return 0x03u;
        case 0x2666u: return 0x04u;
        case 0x266Au: return 0x0Du;
        case 0x266Bu: return 0x0Eu;
        case 0x0393u: return 0xE2u;
        case 0x0398u: return 0xE9u;
        case 0x03A3u: return 0xE4u;
        case 0x03A6u: return 0xE8u;
        case 0x03A9u: return 0xEAu;
        case 0x03B1u: return 0xE0u;
        case 0x03B4u: return 0xEBu;
        case 0x03B5u: return 0xEEu;
        case 0x03C0u: return 0xE3u;
        case 0x03C3u: return 0xE5u;
        case 0x03C4u: return 0xE7u;
        case 0x03C6u: return 0xEDu;
        case 0x2022u: return 0x07u;
        case 0x2190u: return 0x1Bu;
        case 0x2191u: return 0x18u;
        case 0x2192u: return 0x1Au;
        case 0x2193u: return 0x19u;
        case 0x2194u: return 0x1Du;
        case 0x2195u: return 0x12u;
        case 0x21A8u: return 0x17u;
        case 0x221Au: return 0xFBu;
        case 0x221Eu: return 0xECu;
        case 0x2229u: return 0xEFu;
        case 0x2248u: return 0xF7u;
        case 0x2261u: return 0xF0u;
        case 0x2264u: return 0xF3u;
        case 0x2265u: return 0xF2u;
        case 0x2302u: return 0x7Fu;
        case 0x2310u: return 0xA9u;
        case 0x2320u: return 0xF4u;
        case 0x2321u: return 0xF5u;
        case 0x2500u: return 0xC4u;
        case 0x2502u: return 0xB3u;
        case 0x250Cu: return 0xDAu;
        case 0x2510u: return 0xBFu;
        case 0x2514u: return 0xC0u;
        case 0x2518u: return 0xD9u;
        case 0x251Cu: return 0xC3u;
        case 0x2524u: return 0xB4u;
        case 0x252Cu: return 0xC2u;
        case 0x2534u: return 0xC1u;
        case 0x253Cu: return 0xC5u;
        case 0x2550u: return 0xCDu;
        case 0x2551u: return 0xBAu;
        case 0x2554u: return 0xC9u;
        case 0x2557u: return 0xBBu;
        case 0x255Au: return 0xC8u;
        case 0x255Du: return 0xBCu;
        case 0x2560u: return 0xCCu;
        case 0x2563u: return 0xB9u;
        case 0x2566u: return 0xCBu;
        case 0x2569u: return 0xCAu;
        case 0x256Cu: return 0xCEu;
        case 0x2580u: return 0xDFu;
        case 0x2584u: return 0xDCu;
        case 0x2588u: return 0xDBu;
        case 0x258Cu: return 0xDDu;
        case 0x2590u: return 0xDEu;
        case 0x2591u: return 0xB0u;
        case 0x2592u: return 0xB1u;
        case 0x2593u: return 0xB2u;
        case 0x25A0u: return 0xFEu;
        default: return '?';
    }
}
