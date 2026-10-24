#pragma once
/* Host-only mbedTLS API adapter to OpenSSL AES-256-GCM. */
#include <openssl/evp.h>
#include <stdint.h>
#include <string.h>
#define MBEDTLS_CIPHER_ID_AES 1
#define MBEDTLS_GCM_ENCRYPT 1
typedef struct { uint8_t key[32]; } mbedtls_gcm_context;
static inline void mbedtls_gcm_init(mbedtls_gcm_context *c){memset(c,0,sizeof(*c));}
static inline void mbedtls_gcm_free(mbedtls_gcm_context *c){memset(c,0,sizeof(*c));}
static inline int mbedtls_gcm_setkey(mbedtls_gcm_context *c,int id,const uint8_t *key,unsigned bits){(void)id;if(bits!=256)return -1;memcpy(c->key,key,32);return 0;}
static inline int mbedtls_gcm_crypt_and_tag(mbedtls_gcm_context *c,int mode,size_t n,const uint8_t *iv,size_t ivn,const uint8_t *aad,size_t an,const uint8_t *in,uint8_t *out,size_t tn,uint8_t *tag){
    (void)mode;EVP_CIPHER_CTX *e=EVP_CIPHER_CTX_new();int len=0;
    int ok=EVP_EncryptInit_ex(e,EVP_aes_256_gcm(),NULL,NULL,NULL)==1 && EVP_CIPHER_CTX_ctrl(e,EVP_CTRL_GCM_SET_IVLEN,(int)ivn,NULL)==1 &&
        EVP_EncryptInit_ex(e,NULL,NULL,c->key,iv)==1 && EVP_EncryptUpdate(e,NULL,&len,aad,(int)an)==1 &&
        EVP_EncryptUpdate(e,out,&len,in,(int)n)==1 && EVP_EncryptFinal_ex(e,out+len,&len)==1 && EVP_CIPHER_CTX_ctrl(e,EVP_CTRL_GCM_GET_TAG,(int)tn,tag)==1;
    EVP_CIPHER_CTX_free(e);return ok?0:-1;
}
static inline int mbedtls_gcm_auth_decrypt(mbedtls_gcm_context *c,size_t n,const uint8_t *iv,size_t ivn,const uint8_t *aad,size_t an,const uint8_t *tag,size_t tn,const uint8_t *in,uint8_t *out){
    EVP_CIPHER_CTX *e=EVP_CIPHER_CTX_new();int len=0;
    int ok=EVP_DecryptInit_ex(e,EVP_aes_256_gcm(),NULL,NULL,NULL)==1 && EVP_CIPHER_CTX_ctrl(e,EVP_CTRL_GCM_SET_IVLEN,(int)ivn,NULL)==1 &&
        EVP_DecryptInit_ex(e,NULL,NULL,c->key,iv)==1 && EVP_DecryptUpdate(e,NULL,&len,aad,(int)an)==1 &&
        EVP_DecryptUpdate(e,out,&len,in,(int)n)==1 && EVP_CIPHER_CTX_ctrl(e,EVP_CTRL_GCM_SET_TAG,(int)tn,(void *)tag)==1 && EVP_DecryptFinal_ex(e,out+len,&len)==1;
    EVP_CIPHER_CTX_free(e);return ok?0:-1;
}
