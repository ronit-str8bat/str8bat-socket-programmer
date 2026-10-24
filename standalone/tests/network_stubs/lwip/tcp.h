#pragma once
#include "lwip/ip_addr.h"
#include <stddef.h>
typedef int err_t;
typedef uint16_t u16_t;
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_ABRT -2
#define TCP_WRITE_FLAG_COPY 1
struct pbuf { u16_t tot_len; const void *payload; };
struct tcp_pcb;
typedef err_t (*recv_fn)(void *,struct tcp_pcb *,struct pbuf *,err_t);
typedef void (*error_fn)(void *,err_t);
struct tcp_pcb { void *unsent,*unacked;recv_fn recv;error_fn error; };
struct tcp_pcb *tcp_new_ip_type(int type);
err_t tcp_bind(struct tcp_pcb *p,const ip_addr_t *ip,u16_t port);
struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *p,int backlog);
void tcp_accept(struct tcp_pcb *p,err_t (*fn)(void *,struct tcp_pcb *,err_t));
void tcp_arg(struct tcp_pcb *p,void *arg);
void tcp_recv(struct tcp_pcb *p,recv_fn fn);
void tcp_err(struct tcp_pcb *p,error_fn fn);
void tcp_abort(struct tcp_pcb *p);
err_t tcp_close(struct tcp_pcb *p);
void tcp_recved(struct tcp_pcb *p,u16_t n);
u16_t tcp_sndbuf(struct tcp_pcb *p);
err_t tcp_write(struct tcp_pcb *p,const void *data,u16_t n,int flags);
void tcp_output(struct tcp_pcb *p);
u16_t pbuf_copy_partial(const struct pbuf *p,void *data,u16_t n,u16_t offset);
void pbuf_free(struct pbuf *p);
