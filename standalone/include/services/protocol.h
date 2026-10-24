#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct { char host[128], path[384]; } https_url_t;
typedef struct { uint32_t revision, size; uint8_t sha[32]; https_url_t image; } ota_manifest_t;
bool parse_https_url(const char *text, https_url_t *out);
/* Strict, flat JSON contract: revision, size, sha256, url, target, address.
 * Unknown/duplicate fields, escapes, overflowing integers are rejected. */
bool parse_manifest(const char *text, ota_manifest_t *out);
bool parse_form(const char *body, char *ssid, char *password, char *token);
/* Mutates a complete, NUL-terminated HTTP response header. */
bool parse_http_response(char *header, uint32_t limit, uint32_t *length);
