#ifndef SSL_H
#define SSL_H

#include <openssl/ssl.h>
#include <openssl/err.h>

void initialize_ssl(void);
SSL_CTX *create_ssl_context(void);

#endif // SSL_H