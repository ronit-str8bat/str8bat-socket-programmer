#pragma once
#include <stdint.h>
typedef struct { uint32_t addr; } ip_addr_t;
struct netif { int unused; };
#define IPADDR_TYPE_V4 0
#define IP4_ADDR(p,a,b,c,d) ((p)->addr=((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(d))
#define ip_2_ip4(p) (p)
static inline void netif_set_addr(struct netif *n,const ip_addr_t *a,const ip_addr_t *b,const ip_addr_t *c){(void)n;(void)a;(void)b;(void)c;}
