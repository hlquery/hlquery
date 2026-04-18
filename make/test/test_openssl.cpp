/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 * Copyright (C) 2021-2026 Carlos F. Ferry <carlos.ferry@gmail.com>
 * Released under the BSD 3-Clause License. See https://docs.hlquery.com/
 */

/*
 * OpenSSL Library Tests
 */

#ifdef HLQUERY_HAS_OPENSSL
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#endif

#include <cstring>
#include <iostream>
#include <vector>

int test_openssl()
{
#ifdef HLQUERY_HAS_OPENSSL
     try
     {

          /* Initialize OpenSSL */

          SSL_library_init();
          SSL_load_error_strings();
          OpenSSL_add_all_algorithms();

          /* Test SSL context creation */

          const SSL_METHOD* method = TLS_server_method();

          if (!method)
          {
               return 1;
          }

          SSL_CTX* ctx = SSL_CTX_new(method);

          if (!ctx)
          {
               return 2;
          }

          /* Test random number generation */

          unsigned char buffer[32];

          if (RAND_bytes(buffer, sizeof(buffer)) != 1)
          {
               SSL_CTX_free(ctx);
               return 3;
          }

          /* Test hash functions */

          EVP_MD_CTX* mdctx = EVP_MD_CTX_new();

          if (!mdctx)
          {
               SSL_CTX_free(ctx);
               return 4;
          }

          if (EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL) != 1)
          {
               EVP_MD_CTX_free(mdctx);
               SSL_CTX_free(ctx);
               return 5;
          }

          const char* test_data = "Hello, hlquery!";

          if (EVP_DigestUpdate(mdctx, test_data, strlen(test_data)) != 1)
          {
               EVP_MD_CTX_free(mdctx);
               SSL_CTX_free(ctx);
               return 6;
          }

          unsigned char hash[EVP_MAX_MD_SIZE];
          unsigned int hash_len;

          if (EVP_DigestFinal_ex(mdctx, hash, &hash_len) != 1)
          {
               EVP_MD_CTX_free(mdctx);
               SSL_CTX_free(ctx);
               return 7;
          }

          /* Cleanup */

          EVP_MD_CTX_free(mdctx);
          SSL_CTX_free(ctx);
          EVP_cleanup();

          return 0; /* Success */
     }
     catch (...)
     {
          return 99;
     }
#else
     return 100; /* OpenSSL not available */
#endif
}
