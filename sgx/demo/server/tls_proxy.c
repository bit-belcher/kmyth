/**
 * @file tls_proxy.c
 * @brief Code for the ECDHE/TLS proxy application.
 */

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/conf.h>
#include <openssl/err.h>
#include <openssl/opensslconf.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <poll.h>

#include "ecdh_demo.h"
#include "tls_proxy.h"

#ifndef DEMO_LOG_LEVEL
#define DEMO_LOG_LEVEL LOG_DEBUG
#endif

#define NUM_POLL_FDS 2

void proxy_init(TLSProxy * proxy)
{
  secure_memset(proxy, 0, sizeof(TLSProxy));
  init(&proxy->ecdhconn);
}

static void tls_cleanup(TLSConnection *tlsconn)
{
  if (tlsconn->conn != NULL)
  {
    BIO_free_all(tlsconn->conn);
  }

  if (tlsconn->ctx != NULL)
  {
    SSL_CTX_free(tlsconn->ctx);
  }
}

void proxy_cleanup(TLSProxy * proxy)
{
  cleanup(&proxy->ecdhconn);
  tls_cleanup(&proxy->tlsconn);

  proxy_init(proxy);
}

void proxy_error(TLSProxy * proxy)
{
  proxy_cleanup(proxy);
  exit(EXIT_FAILURE);
}

static void proxy_usage(const char *prog)
{
  fprintf(stdout,
    "\nusage: %s [options]\n\n"
    "options are:\n\n"
    "ECDH Connection Information --\n"
    "  -p or --local-port      The port number to listen on for ECDH connections.\n"
    "  -r or --private         Local private key PEM file used for ECDH connections.\n"
    "  -u or --public          Remote public key PEM file used to validate ECDH connections.\n"
    "TLS Connection Information --\n"
    "  -I or --remote-ip       The IP address or hostname of the remote server.\n"
    "  -P or --remote-port     The port number to use when connecting to the remote server.\n"
    "  -C or --ca-path         Optional certificate file used to verify the remote server (if not specified, the default system CA chain will be used instead).\n"
    "  -R or --client-key      Local private key PEM file used for TLS connections.\n"
    "  -U or --client-cert     Local certificate PEM file used for TLS connections.\n"
    "Test Options --\n"
    "  -m or --maxconn  The number of connections the server will accept before exiting (unlimited by default, or if the value is not a positive integer).\n"
    "Misc --\n"
    "  -h or --help     Help (displays this usage).\n\n", prog);
}

static void proxy_get_options(TLSProxy * proxy, int argc, char **argv)
{
  // Exit early if there are no arguments.
  if (1 == argc)
  {
    proxy_usage(argv[0]);
    exit(EXIT_SUCCESS);
  }

  int options;
  int option_index = 0;

  while ((options =
          getopt_long(argc, argv, "r:u:p:I:P:C:R:U:m:h", longopts, &option_index)) != -1)
  {
    switch (options)
    {
    // Key files
    case 'r':
      proxy->ecdhconn.private_key_path = optarg;
      break;
    case 'u':
      proxy->ecdhconn.public_cert_path = optarg;
      break;
    // ECDH Connection
    case 'p':
      proxy->ecdhconn.port = optarg;
      break;
    // TLS Connection
    case 'I':
      proxy->tlsconn.host = optarg;
      break;
    case 'P':
      proxy->tlsconn.port = optarg;
      break;
    case 'C':
      proxy->tlsconn.ca_path = optarg;
      break;
    case 'R':
      proxy->tlsconn.client_key_path = optarg;
      break;
    case 'U':
      proxy->tlsconn.client_cert_path = optarg;
      break;
    // Test
    case 'm':
      proxy->ecdhconn.maxconn = atoi(optarg);
      break;
    // Misc
    case 'h':
      proxy_usage(argv[0]);
      exit(EXIT_SUCCESS);
    default:
      proxy_error(proxy);
    }
  }
}

void proxy_check_options(TLSProxy * proxy)
{
  check_options(&proxy->ecdhconn);

  bool err = false;

  if (proxy->tlsconn.host == NULL)
  {
    fprintf(stderr, "Remote IP argument (-I) is required.\n");
    err = true;
  }
  if (proxy->tlsconn.port == NULL)
  {
    fprintf(stderr, "Remote port number argument (-P) is required.\n");
    err = true;
  }
  if (err)
  {
    kmyth_log(LOG_ERR, "Invalid command-line arguments.");
    proxy_error(proxy);
  }
}

static void log_openssl_error(unsigned long err, const char* const label)
{
  const char* const str = ERR_reason_error_string(err);
  if (str)
  {
    kmyth_log(LOG_ERR, "%s: %s", label, str);
  }
  else
  {
    kmyth_log(LOG_ERR, "%s failed: %lu (0x%lx)", label, err, err);
  }
}

static int tls_config_ctx(TLSConnection * tlsconn)
{
  int ret;
  unsigned long ssl_err;

  const SSL_METHOD* method = TLS_client_method();
  ssl_err = ERR_get_error();
  if (NULL == method)
  {
    log_openssl_error(ssl_err, "TLS_client_method");
    return -1;
  }

  tlsconn->ctx = SSL_CTX_new(method);
  ssl_err = ERR_get_error();
  if (tlsconn->ctx == NULL)
  {
    log_openssl_error(ssl_err, "SSL_CTX_new");
    return -1;
  }

  /* Disable deprecated TLS versions. */
  ret = SSL_CTX_set_min_proto_version(tlsconn->ctx, TLS1_2_VERSION);
  ssl_err = ERR_get_error();
  if (1 != ret)
  {
    log_openssl_error(ssl_err, "SSL_CTX_set_min_proto_version");
    return -1;
  }

  /* Enable certificate verification. */
  // Can set a callback function here for advanced debugging.
  SSL_CTX_set_verify(tlsconn->ctx, SSL_VERIFY_PEER, NULL);
  SSL_CTX_set_verify_depth(tlsconn->ctx, 5);

  /* Enable custom or default certificate authorities. */
  if (tlsconn->ca_path) {
    ret = SSL_CTX_load_verify_locations(tlsconn->ctx, tlsconn->ca_path, NULL);
    ssl_err = ERR_get_error();
    if (1 != ret)
    {
      log_openssl_error(ssl_err, "SSL_CTX_load_verify_locations");
      return -1;
    }
  }
  else
  {
    ret = SSL_CTX_set_default_verify_paths(tlsconn->ctx);
    ssl_err = ERR_get_error();
    if (1 != ret)
    {
      log_openssl_error(ssl_err, "SSL_CTX_set_default_verify_paths");
      return -1;
    }
  }

  /* Set client key - required by some servers. */
  if (tlsconn->client_key_path)
  {
    ret = SSL_CTX_use_PrivateKey_file(tlsconn->ctx, tlsconn->client_key_path, SSL_FILETYPE_PEM);
    ssl_err = ERR_get_error();
    if (1 != ret)
    {
      log_openssl_error(ssl_err, "SSL_CTX_use_PrivateKey_file");
      return -1;
    }
  }

  /* Set client cert - required by some servers. */
  if (tlsconn->client_cert_path)
  {
    ret = SSL_CTX_use_certificate_file(tlsconn->ctx, tlsconn->client_cert_path, SSL_FILETYPE_PEM);
    ssl_err = ERR_get_error();
    if (1 != ret)
    {
      log_openssl_error(ssl_err, "SSL_CTX_use_certificate_file");
      return -1;
    }
  }

  return 0;
}

static int tls_config_conn(TLSConnection * tlsconn)
{
  int ret;
  unsigned long ssl_err;
  SSL *ssl = NULL;

  tlsconn->conn = BIO_new_ssl_connect(tlsconn->ctx);
  ssl_err = ERR_get_error();
  if (tlsconn->conn == NULL)
  {
    log_openssl_error(ssl_err, "BIO_new_ssl_connect");
    return -1;
  }

  ret = BIO_set_conn_hostname(tlsconn->conn, tlsconn->host);
  ssl_err = ERR_get_error();
  if (1 != ret)
  {
    log_openssl_error(ssl_err, "BIO_set_conn_hostname");
    return -1;
  }

  ret = BIO_set_conn_port(tlsconn->conn, tlsconn->port);
  ssl_err = ERR_get_error();
  if (1 != ret)
  {
    log_openssl_error(ssl_err, "BIO_set_conn_port");
    return -1;
  }

  BIO_get_ssl(tlsconn->conn, &ssl);  // internal pointer, not a new allocation
  ssl_err = ERR_get_error();
  if (ssl == NULL)
  {
    log_openssl_error(ssl_err, "BIO_get_ssl");
    return -1;
  }

  /* Set hostname for Server Name Indication. */
  ret = SSL_set_tlsext_host_name(ssl, tlsconn->host);
  ssl_err = ERR_get_error();
  if (1 != ret)
  {
    log_openssl_error(ssl_err, "SSL_set_tlsext_host_name");
    return -1;
  }

  /* Set hostname for certificate verification. */
  ret = SSL_set1_host(ssl, tlsconn->host);
  ssl_err = ERR_get_error();
  if (1 != ret)
  {
    log_openssl_error(ssl_err, "SSL_set1_host");
    return -1;
  }

  return 0;
}

static void tls_get_verify_error(TLSConnection * tlsconn)
{
  int ret;
  unsigned long ssl_err;
  SSL *ssl = NULL;

  BIO_get_ssl(tlsconn->conn, &ssl);  // internal pointer, not a new allocation
  ssl_err = ERR_get_error();
  if (ssl == NULL)
  {
    log_openssl_error(ssl_err, "BIO_get_ssl");
    return;
  }

  ret = SSL_get_verify_result(ssl);
  if (X509_V_OK != ret)
  {
    kmyth_log(LOG_ERR, "SSL_get_verify_result: %s",
              X509_verify_cert_error_string(ret));
  }
}

static int tls_connect(TLSConnection * tlsconn)
{
  int ret;
  unsigned long ssl_err;

  ret = BIO_do_connect(tlsconn->conn);
  ssl_err = ERR_get_error();
  if (1 != ret)
  {
    /* Both connection failures and certificate verification failures are caught here. */
    log_openssl_error(ssl_err, "BIO_do_connect");
    tls_get_verify_error(tlsconn);
    return -1;
  }

  return 0;
}

static int setup_ecdhconn(TLSProxy * proxy)
{
  ECDHServer *ecdhconn = &proxy->ecdhconn;

  create_server_socket(ecdhconn);

  load_private_key(ecdhconn);
  load_public_key(ecdhconn);

  make_ephemeral_keypair(ecdhconn);

  recv_ephemeral_public(ecdhconn);
  send_ephemeral_public(ecdhconn);

  get_session_key(ecdhconn);

  return 0;
}

static int setup_tlsconn(TLSProxy * proxy)
{
  TLSConnection *tlsconn = &proxy->tlsconn;

  if (tls_config_ctx(tlsconn))
  {
    proxy_error(proxy);
  }

  if (tls_config_conn(tlsconn))
  {
    proxy_error(proxy);
  }

  if (tls_connect(tlsconn))
  {
    proxy_error(proxy);
  }

  return 0;
}

void proxy_start(TLSProxy * proxy)
{
  struct pollfd pfds[NUM_POLL_FDS];
  int bytes_read = 0;
  int bytes_written = 0;
  unsigned char tls_msg_buf[ECDH_MAX_MSG_SIZE];
  unsigned char *ecdh_msg_buf = NULL;
  size_t ecdh_msg_len = 0;
  ECDHServer *ecdhconn = &proxy->ecdhconn;
  BIO *tls_bio = proxy->tlsconn.conn;

  secure_memset(pfds, 0, sizeof(pfds));
  secure_memset(tls_msg_buf, 0, sizeof(tls_msg_buf));

  pfds[0].fd = ecdhconn->socket_fd;
  pfds[0].events = POLLIN;

  pfds[1].fd = BIO_get_fd(tls_bio, NULL);
  pfds[1].events = POLLIN;

  kmyth_log(LOG_DEBUG, "Starting proxy loop");
  while (true)
  {
    /* Wait to receive data with no timeout. */
    poll(pfds, NUM_POLL_FDS, -1);

    if (pfds[0].revents & POLLIN)
    {
      ecdh_recv_decrypt(ecdhconn, &ecdh_msg_buf, &ecdh_msg_len);
      kmyth_log(LOG_DEBUG, "Received %zu bytes on ECDH connection", ecdh_msg_len);
      bytes_written = BIO_write(tls_bio, ecdh_msg_buf, ecdh_msg_len);
      if (bytes_written != ecdh_msg_len)
      {
        kmyth_log(LOG_ERR, "TLS write error");
        proxy_error(proxy);
      }
      kmyth_clear_and_free(ecdh_msg_buf, ecdh_msg_len);
    }

    if (pfds[1].revents & POLLIN)
    {
      bytes_read = BIO_read(proxy->tlsconn.conn, tls_msg_buf, sizeof(tls_msg_buf));
      if (bytes_read == 0)
      {
        kmyth_log(LOG_INFO, "TLS connection is closed");
        break;
      }
      else if (bytes_read < 0)
      {
        kmyth_log(LOG_ERR, "TLS read error");
        proxy_error(proxy);
      }
      kmyth_log(LOG_DEBUG, "Received %zu bytes on TLS connection", bytes_read);
      ecdh_encrypt_send(ecdhconn, tls_msg_buf, bytes_read);
    }
  }
}

void proxy_main(TLSProxy * proxy)
{
  // The ECDH setup must come first because it forks a new process to handle each new connection.
  setup_ecdhconn(proxy);
  setup_tlsconn(proxy);
  proxy_start(proxy);
}

int main(int argc, char **argv)
{
  TLSProxy proxy;

  proxy_init(&proxy);

  set_applog_severity_threshold(DEMO_LOG_LEVEL);

  proxy_get_options(&proxy, argc, argv);
  proxy_check_options(&proxy);

  proxy_main(&proxy);

  proxy_cleanup(&proxy);

  return EXIT_SUCCESS;
}
