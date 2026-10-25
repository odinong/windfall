#pragma once
#include <Windows.h>
#include <bcrypt.h>

#define SHA_DIGEST_LENGTH 20
#define SHA256_DIGEST_LENGTH 32

struct EVP_MD { BCRYPT_ALG_HANDLE alg; unsigned len; };
struct EVP_MD_CTX { BCRYPT_HASH_HANDLE h = nullptr; const EVP_MD* md = nullptr; };

inline const EVP_MD* EVP_sha1() { static const EVP_MD md{BCRYPT_SHA1_ALG_HANDLE, 20}; return &md; }
inline const EVP_MD* EVP_sha256() { static const EVP_MD md{BCRYPT_SHA256_ALG_HANDLE, 32}; return &md; }
inline EVP_MD_CTX* EVP_MD_CTX_new() { return new EVP_MD_CTX; }
inline void EVP_MD_CTX_free(EVP_MD_CTX* c)
{
    if (c && c->h) BCryptDestroyHash(c->h);
    delete c;
}
inline int EVP_DigestInit(EVP_MD_CTX* c, const EVP_MD* md)
{
    if (c->h) BCryptDestroyHash(c->h);
    c->md = md;
    return BCryptCreateHash(md->alg, &c->h, nullptr, 0, nullptr, 0, 0) == 0;
}
inline int EVP_DigestInit_ex(EVP_MD_CTX* c, const EVP_MD* md, void*) { return EVP_DigestInit(c, md); }
inline int EVP_DigestUpdate(EVP_MD_CTX* c, const void* d, size_t n) { return BCryptHashData(c->h, (PUCHAR)d, (ULONG)n, 0) == 0; }
inline int EVP_DigestFinal_ex(EVP_MD_CTX* c, unsigned char* out, unsigned int* len)
{
    if (len) *len = c->md->len;
    return BCryptFinishHash(c->h, out, c->md->len, 0) == 0;
}
inline int EVP_Digest(const void* d, size_t n, unsigned char* out, unsigned int* len, const EVP_MD* md, void*)
{
    if (len) *len = md->len;
    return BCryptHash(md->alg, nullptr, 0, (PUCHAR)d, (ULONG)n, out, md->len) == 0;
}
inline unsigned char* SHA1(const unsigned char* d, size_t n, unsigned char* out) { EVP_Digest(d, n, out, nullptr, EVP_sha1(), nullptr); return out; }
inline unsigned char* SHA256(const unsigned char* d, size_t n, unsigned char* out) { EVP_Digest(d, n, out, nullptr, EVP_sha256(), nullptr); return out; }
