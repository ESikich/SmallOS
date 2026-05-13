#ifndef UNICODE_H
#define UNICODE_H

#define UNICODE_REPLACEMENT_CHAR 0xFFFDu

typedef struct {
    unsigned int codepoint;
    unsigned int min_codepoint;
    unsigned char remaining;
} utf8_decoder_t;

typedef enum {
    UTF8_DECODE_WAIT = 0,
    UTF8_DECODE_ACCEPT = 1,
    UTF8_DECODE_REJECT = 2,
    UTF8_DECODE_REJECT_RETRY = 3,
} utf8_decode_result_t;

void utf8_decoder_reset(utf8_decoder_t* dec);
utf8_decode_result_t utf8_decoder_feed(utf8_decoder_t* dec,
                                       unsigned char byte,
                                       unsigned int* out_codepoint);

unsigned char unicode_to_cp437(unsigned int codepoint);

#endif
