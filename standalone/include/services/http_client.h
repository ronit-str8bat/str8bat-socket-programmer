#pragma once
#include "services/protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef bool (*http_sink_fn)(const void *,size_t);
typedef int (*http_source_fn)(uint32_t,void *,size_t);
/* One client, strict HTTPS on port 443. Body length is mandatory; redirects,
 * compression and chunked encoding are rejected. poll() alone calls sinks. */
bool http_start(const https_url_t *url,const char *batch_id,uint32_t upload_size,
                http_source_fn source,uint32_t max_response,http_sink_fn sink);
void http_poll(void);
void http_cancel(void);
/* 0=running, 1=HTTP 200 complete, -1=failed */
int http_result(void);
uint32_t http_body_size(void);
