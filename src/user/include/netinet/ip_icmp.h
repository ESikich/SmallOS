#ifndef USER_NETINET_IP_ICMP_H
#define USER_NETINET_IP_ICMP_H

#include "ip.h"
#include "../stdint.h"

#define ICMP_ECHOREPLY      0
#define ICMP_DEST_UNREACH   3
#define ICMP_SOURCE_QUENCH  4
#define ICMP_REDIRECT       5
#define ICMP_ECHO           8
#define ICMP_TIME_EXCEEDED  11
#define ICMP_PARAMETERPROB  12
#define ICMP_TIMESTAMP      13
#define ICMP_TIMESTAMPREPLY 14
#define ICMP_INFO_REQUEST   15
#define ICMP_INFO_REPLY     16
#define ICMP_ADDRESS        17
#define ICMP_ADDRESSREPLY   18

#define ICMP_MINLEN 8

struct icmp {
    uint8_t icmp_type;
    uint8_t icmp_code;
    uint16_t icmp_cksum;
    union {
        struct {
            uint16_t id;
            uint16_t seq;
        } echo;
        uint32_t word;
        uint8_t data[4];
    } icmp_hun;
    union {
        uint8_t data[1];
        uint32_t timestamp;
    } icmp_dun;
};

#define icmp_id   icmp_hun.echo.id
#define icmp_seq  icmp_hun.echo.seq
#define icmp_data icmp_dun.data

struct icmphdr {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    union {
        struct {
            uint16_t id;
            uint16_t sequence;
        } echo;
        uint32_t gateway;
    } un;
};

#endif /* USER_NETINET_IP_ICMP_H */
