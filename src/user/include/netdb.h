#ifndef USER_NETDB_H
#define USER_NETDB_H

#include "sys/socket.h"

struct hostent {
    char* h_name;
    char** h_aliases;
    int h_addrtype;
    int h_length;
    char** h_addr_list;
};

#define h_addr h_addr_list[0]

struct servent {
    char* s_name;
    char** s_aliases;
    int s_port;
    char* s_proto;
};

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr* ai_addr;
    char* ai_canonname;
    struct addrinfo* ai_next;
};

#define AI_PASSIVE 0x01
#define AI_CANONNAME 0x02
#define AI_NUMERICHOST 0x04
#define AI_NUMERICSERV 0x08

#define EAI_NONAME -2
#define EAI_FAMILY -6
#define EAI_FAIL -4
#define EAI_SERVICE -8
#define EAI_SYSTEM -11

#define NI_NUMERICHOST 0x01
#define NI_NUMERICSERV 0x02
#define NI_NOFQDN 0x04
#define NI_NAMEREQD 0x08
#define NI_DGRAM 0x10

#define HOST_NOT_FOUND 1
#define TRY_AGAIN 2
#define NO_RECOVERY 3
#define NO_DATA 4
#define NO_ADDRESS NO_DATA

extern int h_errno;

struct hostent* gethostbyname(const char* name);
struct servent* getservbyname(const char* name, const char* proto);
const char* hstrerror(int err);
int getaddrinfo(const char* node, const char* service,
                const struct addrinfo* hints, struct addrinfo** res);
int getnameinfo(const struct sockaddr* sa, socklen_t salen,
                char* host, socklen_t hostlen,
                char* serv, socklen_t servlen, int flags);
void freeaddrinfo(struct addrinfo* res);
const char* gai_strerror(int errcode);

#endif /* USER_NETDB_H */
