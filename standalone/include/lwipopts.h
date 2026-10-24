#pragma once
#define NO_SYS 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_DHCP 1
#define LWIP_DNS 1
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_ICMP 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define MEM_ALIGNMENT 4
#define MEM_SIZE (48*1024)
#define MEMP_NUM_TCP_PCB 6
#define MEMP_NUM_TCP_SEG 48
#define PBUF_POOL_SIZE 32
#define TCP_MSS 1460
#define TCP_WND (12*TCP_MSS)
#define TCP_SND_BUF (8*TCP_MSS)
#define TCP_SND_QUEUELEN 32
#define LWIP_ALTCP 1
#define LWIP_ALTCP_TLS 1
#define LWIP_ALTCP_TLS_MBEDTLS 1
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_STATS 0
#define LWIP_CHKSUM_ALGORITHM 3
#define SNTP_SERVER_DNS 1
#define SNTP_CHECK_RESPONSE 2
#define ALTCP_MBEDTLS_AUTHMODE MBEDTLS_SSL_VERIFY_REQUIRED
/* Random request timestamps act as echoed nonces; round-trip compensation
 * must remain disabled when these timestamps are deliberately random. */
#define SNTP_COMP_ROUNDTRIP 0
unsigned long services_ntp_nonce(void);
#define SNTP_GET_SYSTEM_TIME_NTP(sec,frac) do { (sec) = (long)services_ntp_nonce(); (frac) = services_ntp_nonce(); } while (0)
void services_set_ntp_time(unsigned long seconds);
#define SNTP_SET_SYSTEM_TIME(sec) services_set_ntp_time(sec)
