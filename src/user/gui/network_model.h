#ifndef SMALLOS_GUI_NETWORK_MODEL_H
#define SMALLOS_GUI_NETWORK_MODEL_H

typedef enum gui_ipv4_validation {
    GUI_IPV4_VALID = 0,
    GUI_IPV4_INVALID_ADDRESS,
    GUI_IPV4_INVALID_PREFIX,
    GUI_IPV4_INVALID_GATEWAY,
    GUI_IPV4_INVALID_DNS,
    GUI_IPV4_NETWORK_OR_BROADCAST,
    GUI_IPV4_GATEWAY_OUTSIDE_SUBNET,
} gui_ipv4_validation_t;

typedef struct gui_ipv4_config {
    unsigned int address;
    unsigned int prefix;
    unsigned int netmask;
    unsigned int gateway;
    unsigned int dns;
} gui_ipv4_config_t;

int gui_ipv4_parse(const char* text, unsigned int* output);
int gui_ipv4_mask_prefix(unsigned int mask, unsigned int* prefix);
gui_ipv4_validation_t gui_ipv4_validate(const char* address,
                                         const char* prefix,
                                         const char* gateway,
                                         const char* dns,
                                         gui_ipv4_config_t* output);

#endif
