#ifndef TCP_H
#define TCP_H

/*
 * Minimal kernel TCP bring-up.
 *
 * The first cut is a single passive listener that accepts one connection
 * at a time and echoes received payload back to the peer.  It runs as a
 * background kernel task so the rest of the system can keep booting and
 * the existing test suite stays unchanged.
 */

void tcp_init(void);

#endif /* TCP_H */
