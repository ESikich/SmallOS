#include "network_model.h"

int gui_ipv4_parse(const char* text, unsigned int* output) {
    unsigned int address = 0;
    unsigned int count = 0;
    const char* p = text;
    if (!p || !*p) return 0;
    while (count < 4u) {
        unsigned int value = 0;
        unsigned int digits = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10u + (unsigned int)(*p++ - '0');
            if (++digits > 3u || value > 255u) return 0;
        }
        if (!digits) return 0;
        address = (address << 8) | value;
        count++;
        if (count == 4u) break;
        if (*p++ != '.') return 0;
    }
    if (*p) return 0;
    if (output) *output = address;
    return 1;
}

static int parse_prefix(const char* text, unsigned int* output) {
    unsigned int value = 0;
    const char* p = text;
    if (!p || !*p) return 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10u + (unsigned int)(*p++ - '0');
        if (value > 30u) return 0;
    }
    if (*p || value < 1u) return 0;
    *output = value;
    return 1;
}

int gui_ipv4_mask_prefix(unsigned int mask, unsigned int* prefix) {
    unsigned int count = 0;
    int saw_zero = 0;
    for (unsigned int bit = 0; bit < 32u; bit++) {
        if (mask & (0x80000000u >> bit)) {
            if (saw_zero) return 0;
            count++;
        } else {
            saw_zero = 1;
        }
    }
    if (prefix) *prefix = count;
    return 1;
}

gui_ipv4_validation_t gui_ipv4_validate(const char* address,
                                         const char* prefix_text,
                                         const char* gateway,
                                         const char* dns,
                                         gui_ipv4_config_t* output) {
    gui_ipv4_config_t config = {0, 0, 0, 0, 0};
    unsigned int host;
    if (!gui_ipv4_parse(address, &config.address))
        return GUI_IPV4_INVALID_ADDRESS;
    if (!parse_prefix(prefix_text, &config.prefix))
        return GUI_IPV4_INVALID_PREFIX;
    if (gateway && *gateway && !gui_ipv4_parse(gateway, &config.gateway))
        return GUI_IPV4_INVALID_GATEWAY;
    if (dns && *dns && !gui_ipv4_parse(dns, &config.dns))
        return GUI_IPV4_INVALID_DNS;
    config.netmask = 0xffffffffu << (32u - config.prefix);
    host = config.address & ~config.netmask;
    if (host == 0u || host == ~config.netmask)
        return GUI_IPV4_NETWORK_OR_BROADCAST;
    if (config.gateway &&
        (config.gateway & config.netmask) !=
        (config.address & config.netmask))
        return GUI_IPV4_GATEWAY_OUTSIDE_SUBNET;
    if (output) *output = config;
    return GUI_IPV4_VALID;
}
