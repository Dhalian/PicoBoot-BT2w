/**
 * lwipopts.h
 *
 * Minimal lwIP configuration for our tiny embedded HTTP server. Based on
 * the standard starter config used across the official pico-examples
 * Pico W networking samples, trimmed down for our actual needs: one
 * small HTTP server, a handful of short-lived connections, no need for
 * large buffers or many simultaneous sockets.
 *
 * NO_SYS=1 (set by the pico_cyw43_arch_lwip_threadsafe_background
 * library we link against) means we use lwIP's raw (callback-based) API,
 * not full BSD sockets -- see http_server.c.
 */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS 1
#define LWIP_SOCKET 0
#define MEM_ALIGNMENT 4

#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 1

#define TCP_WND (4 * TCP_MSS)
#define TCP_MSS 1460
#define TCP_SND_BUF (4 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

#define MEM_SIZE 16000

#define MEMP_NUM_TCP_SEG 32
#define MEMP_NUM_ARP_QUEUE 10

#define LWIP_DHCP 1
#define LWIP_IPV4 1
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_DNS 1

#define LWIP_TCP_KEEPALIVE 1

#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1

#define LWIP_NETCONN 0

#define MEM_LIBC_MALLOC 0
#define MEMP_MEM_MALLOC 0

#define LWIP_STATS 0
#define LWIP_STATS_DISPLAY 0

#define PBUF_POOL_SIZE 24

#ifndef NDEBUG
#define LWIP_DEBUG 0
#endif

#define LWIP_CHKSUM_ALGORITHM 3

#endif // LWIPOPTS_H
