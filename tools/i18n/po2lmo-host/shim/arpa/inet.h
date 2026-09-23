/*
 * Host-build shim: <arpa/inet.h>, of which lmo.c/lmo.h need exactly two
 * functions, htonl() and ntohl().  Windows keeps those in <winsock2.h>, which
 * would drag the whole socket layer into a file format tool, so they are
 * spelled out here instead.
 *
 * The .lmo index is defined as big-endian on the wire ("network order"): the
 * writer stores htonl() and both readers call ntohl().  The toolchain this
 * builds on is little-endian, so the two are the same byte swap -- asserted
 * below rather than assumed, because on a big-endian host they must be the
 * identity and a silent swap would corrupt every offset.
 */
#ifndef FM160_SHIM_ARPA_INET_H
#define FM160_SHIM_ARPA_INET_H

#include <stdint.h>

#if !defined(__BYTE_ORDER__) || !defined(__ORDER_LITTLE_ENDIAN__)
#error "cannot determine byte order; this shim needs __BYTE_ORDER__"
#endif

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

static inline uint16_t fm160_bswap16(uint16_t x)
{
	return (uint16_t)((x << 8) | (x >> 8));
}

static inline uint32_t fm160_bswap32(uint32_t x)
{
	return ((x & 0x000000FFu) << 24) |
	       ((x & 0x0000FF00u) <<  8) |
	       ((x & 0x00FF0000u) >>  8) |
	       ((x & 0xFF000000u) >> 24);
}

#else

static inline uint16_t fm160_bswap16(uint16_t x) { return x; }
static inline uint32_t fm160_bswap32(uint32_t x) { return x; }

#endif

static inline uint32_t htonl(uint32_t x) { return fm160_bswap32(x); }
static inline uint32_t ntohl(uint32_t x) { return fm160_bswap32(x); }
static inline uint16_t htons(uint16_t x) { return fm160_bswap16(x); }
static inline uint16_t ntohs(uint16_t x) { return fm160_bswap16(x); }

#endif /* FM160_SHIM_ARPA_INET_H */
