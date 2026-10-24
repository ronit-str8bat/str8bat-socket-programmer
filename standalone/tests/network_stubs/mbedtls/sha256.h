#pragma once
/* Real SHA-256 through OpenSSL on the host. The device uses SDK mbedTLS. */
#include <openssl/evp.h>
#include <stddef.h>
typedef struct { EVP_MD_CTX *ctx; } mbedtls_sha256_context;
static inline void mbedtls_sha256_init(mbedtls_sha256_context *c){c->ctx=EVP_MD_CTX_new();}
static inline int mbedtls_sha256_starts(mbedtls_sha256_context *c,int x){(void)x;return EVP_DigestInit_ex(c->ctx,EVP_sha256(),NULL)==1?0:-1;}
static inline int mbedtls_sha256_update(mbedtls_sha256_context *c,const unsigned char *p,size_t n){return EVP_DigestUpdate(c->ctx,p,n)==1?0:-1;}
static inline int mbedtls_sha256_finish(mbedtls_sha256_context *c,unsigned char out[32]){return EVP_DigestFinal_ex(c->ctx,out,NULL)==1?0:-1;}
static inline void mbedtls_sha256_free(mbedtls_sha256_context *c){EVP_MD_CTX_free(c->ctx);}
static inline int mbedtls_sha256(const unsigned char *p,size_t n,unsigned char out[32],int x){(void)x;return EVP_Digest(p,n,out,NULL,EVP_sha256(),NULL)==1?0:-1;}
