/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 * You are free to redistribute and/or modify this software
 * under the terms of the BSD License.
 * For more details, please visit: https://docs.hlquery.com
 */

#include <sstream>

#include "common/cryptoutils.h"

#ifdef HLQUERY_HAS_OPENSSL

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/md5.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#endif

std::vector<uint8_t> SHA256(const void *data, size_t len)
{
     std::vector<uint8_t> Out(32);

#ifdef HLQUERY_HAS_OPENSSL

     ::SHA256(static_cast<const unsigned char *>(data), len, Out.data());

#else

     /* Fallback: very weak placeholder (not for production) */

     uint32_t Acc = 0x12345678u;

     const uint8_t *P = static_cast<const uint8_t *>(data);

     for (size_t i = 0; i < len; ++i)
     {
          Acc = Acc * 1664525u + P[i] + 1013904223u;
     }

     for (size_t i = 0; i < Out.size(); ++i)
     {
          Out[i] = static_cast<uint8_t>((Acc >> (i % 24)) & 0xFF);
     }

#endif

     return Out;
}

std::vector<uint8_t> HMACSHA256(const void *key, size_t keylen, const void *data, size_t len)
{
     std::vector<uint8_t> Out(32);

#ifdef HLQUERY_HAS_OPENSSL

     unsigned int OutLen = 0;

     unsigned char *Res = ::HMAC(EVP_sha256(), key, static_cast<int>(keylen),
                                 static_cast<const unsigned char *>(data), len,
                                 Out.data(), &OutLen);
     (void)Res;

#else

     /* Fallback: XOR key into data then hash */

     std::vector<uint8_t> Buf(len);

     const uint8_t *K = static_cast<const uint8_t *>(key);

     const uint8_t *D = static_cast<const uint8_t *>(data);

     for (size_t i = 0; i < len; ++i)
     {
          Buf[i] = D[i] ^ K[i % keylen];
     }

     Out = SHA256(Buf.data(), Buf.size());

#endif

     return Out;
}

std::vector<uint8_t> RandomBytes(size_t len)
{
     std::vector<uint8_t> Out(len);

#ifdef HLQUERY_HAS_OPENSSL

     ::RAND_bytes(Out.data(), static_cast<int>(len));

#else

     /* Fallback: very weak PRNG */

     uint32_t S = 0xC001D00Du;

     for (size_t i = 0; i < len; ++i)
     {
          S = S * 1103515245u + 12345u;

          Out[i] = static_cast<uint8_t>(S >> 16);
     }

#endif

     return Out;
}

std::string Hex(const std::vector<uint8_t> &bytes)
{
     std::ostringstream OSS;

     OSS.setf(std::ios::hex, std::ios::basefield);
     OSS.setf(std::ios::right, std::ios::adjustfield);

     for (auto B : bytes)
     {
          OSS.width(2);
          OSS.fill('0');
          OSS << static_cast<int>(B);
     }

     return OSS.str();
}

std::string MD5(const std::string &input)
{
#ifdef HLQUERY_HAS_OPENSSL

     unsigned char Hash[MD5_DIGEST_LENGTH];

     /* Use EVP API (OpenSSL 3.0+ compatible, avoids deprecation warning) */

     EVP_MD_CTX *CTX = EVP_MD_CTX_new();

     if (!CTX)
     {
          return "";
     }

     if (EVP_DigestInit_ex(CTX, EVP_md5(), nullptr) != 1 ||
         EVP_DigestUpdate(CTX, input.c_str(), input.length()) != 1 ||
         EVP_DigestFinal_ex(CTX, Hash, nullptr) != 1)
     {
          EVP_MD_CTX_free(CTX);

          return "";
     }

     EVP_MD_CTX_free(CTX);

     std::ostringstream OSS;

     OSS.setf(std::ios::hex, std::ios::basefield);
     OSS.setf(std::ios::right, std::ios::adjustfield);

     for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
     {
          OSS.width(2);
          OSS.fill('0');
          OSS << static_cast<int>(Hash[i]);
     }

     return OSS.str();

#else

     /* Fallback implementation (simple hash for testing) */

     std::hash<std::string> Hasher;

     std::ostringstream OSS;

     OSS << std::hex << Hasher(input);

     return OSS.str();

#endif
}

std::string AES256Encrypt(const std::string &plaintext, const std::string &key)
{
#ifdef HLQUERY_HAS_OPENSSL

     /* Generate random IV (16 bytes for AES) */

     unsigned char IV[16];

     if (!RAND_bytes(IV, sizeof(IV)))
     {
          return "";
     }

     /* Derive 256-bit key from input key using SHA256 */

     std::vector<uint8_t> KeyHash = SHA256(key.data(), key.size());

     /* Create cipher context */

     EVP_CIPHER_CTX *CTX = EVP_CIPHER_CTX_new();

     if (!CTX)
     {
          return "";
     }

     /* Initialize encryption */

     if (EVP_EncryptInit_ex(CTX, EVP_aes_256_cbc(), nullptr, KeyHash.data(), IV) != 1)
     {
          EVP_CIPHER_CTX_free(CTX);

          return "";
     }

     /* Allocate output buffer (plaintext + block size for padding) */

     std::vector<unsigned char> CipherText(plaintext.size() + EVP_CIPHER_block_size(EVP_aes_256_cbc()));

     int OutLen1 = 0;

     int OutLen2 = 0;

     /* Encrypt */

     if (EVP_EncryptUpdate(CTX, CipherText.data(), &OutLen1,
                           reinterpret_cast<const unsigned char *>(plaintext.data()),
                           plaintext.size()) != 1)
     {
          EVP_CIPHER_CTX_free(CTX);

          return "";
     }

     /* Finalize encryption */

     if (EVP_EncryptFinal_ex(CTX, CipherText.data() + OutLen1, &OutLen2) != 1)
     {
          EVP_CIPHER_CTX_free(CTX);

          return "";
     }

     EVP_CIPHER_CTX_free(CTX);

     /* Prepend IV to ciphertext (IV:CIPHERTEXT) */

     std::string Result;

     Result.append(reinterpret_cast<const char *>(IV), sizeof(IV));
     Result.append(reinterpret_cast<const char *>(CipherText.data()), OutLen1 + OutLen2);

     return Result;

#else

     /* Fallback: XOR with key (NOT SECURE, for testing only) */

     std::string Result = plaintext;

     for (size_t i = 0; i < Result.size(); ++i)
     {
          Result[i] ^= key[i % key.size()];
     }

     return Result;

#endif
}

std::string AES256Decrypt(const std::string &ciphertext, const std::string &key)
{
#ifdef HLQUERY_HAS_OPENSSL

     if (ciphertext.size() < 16)
     {
          return ""; /* Too short to contain IV */
     }

     /* Extract IV (first 16 bytes) */

     const unsigned char *IV = reinterpret_cast<const unsigned char *>(ciphertext.data());

     const unsigned char *EncryptedData = IV + 16;

     size_t EncryptedLen = ciphertext.size() - 16;

     /* Derive 256-bit key from input key using SHA256 */

     std::vector<uint8_t> KeyHash = SHA256(key.data(), key.size());

     /* Create cipher context */

     EVP_CIPHER_CTX *CTX = EVP_CIPHER_CTX_new();

     if (!CTX)
     {
          return "";
     }

     /* Initialize decryption */

     if (EVP_DecryptInit_ex(CTX, EVP_aes_256_cbc(), nullptr, KeyHash.data(), IV) != 1)
     {
          EVP_CIPHER_CTX_free(CTX);

          return "";
     }

     /* Allocate output buffer */

     std::vector<unsigned char> PlainText(EncryptedLen + EVP_CIPHER_block_size(EVP_aes_256_cbc()));

     int OutLen1 = 0;

     int OutLen2 = 0;

     /* Decrypt */

     if (EVP_DecryptUpdate(CTX, PlainText.data(), &OutLen1, EncryptedData, EncryptedLen) != 1)
     {
          EVP_CIPHER_CTX_free(CTX);

          return "";
     }

     /* Finalize decryption */

     if (EVP_DecryptFinal_ex(CTX, PlainText.data() + OutLen1, &OutLen2) != 1)
     {
          EVP_CIPHER_CTX_free(CTX);

          return "";
     }

     EVP_CIPHER_CTX_free(CTX);

     return std::string(reinterpret_cast<const char *>(PlainText.data()), OutLen1 + OutLen2);

#else

     /* Fallback: XOR with key (NOT SECURE, for testing only) */

     std::string Result = ciphertext;

     for (size_t i = 0; i < Result.size(); ++i)
     {
          Result[i] ^= key[i % key.size()];
     }

     return Result;

#endif
}
