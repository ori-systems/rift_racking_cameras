/*
 * @file user_settings.h
 * @brief Configure the wolfSSH and wolfCrypt features used by the console.
 * @author Ajay Varghese
 * @date 2026-11-26
*/

#ifndef RIFT_WOLFSSL_USER_SETTINGS_H
#define RIFT_WOLFSSL_USER_SETTINGS_H

/* Cryptography */

#define WOLFCRYPT_ONLY
#define WOLFSSL_WOLFSSH
#define SINGLE_THREADED
#define NO_FILESYSTEM
#define NO_RSA
#define NO_DH
#define NO_DSA
#define NO_PWDBASED
#define NO_DES3
#define NO_MD5
#define NO_SHA
#define NO_SHA512
#define HAVE_ECC
#define ECC_TIMING_RESISTANT
#define WOLFSSL_SP_MATH_ALL
#define WOLFSSL_HAVE_SP_ECC
#define WOLFSSL_SP_SMALL
#define WOLFSSL_AES_COUNTER
#define HAVE_AESGCM
#define WOLFSSL_SMALL_STACK

/* SSH connection */

#define WOLFSSH_LWIP
#define WOLFSSH_NO_AES_CBC
#define WOLFSSH_TERM
#define NO_TERMIOS

/* Random numbers */

#define CUSTOM_RAND_GENERATE_BLOCK ssh_random_block

/*
 * @brief Fill a buffer with random bytes from the ESP32-P4.
 * @param output Buffer to fill.
 * @param size Number of bytes to write.
 * @return 0 on success.
*/
int ssh_random_block(unsigned char *output, unsigned int size);

#endif
