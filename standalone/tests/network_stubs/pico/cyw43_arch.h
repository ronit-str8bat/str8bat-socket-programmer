#pragma once
#include "lwip/ip_addr.h"
#define CYW43_ITF_AP 1
extern struct test_cyw43 { struct netif netif[2]; } cyw43_state;
#define CYW43_AUTH_WPA2_AES_PSK 0x400004u
void cyw43_arch_enable_ap_mode(const char *ssid,const char *password,unsigned auth);
void cyw43_arch_poll(void);
void cyw43_wifi_ap_get_stas(struct test_cyw43 *self,int *count,uint8_t *mac);
