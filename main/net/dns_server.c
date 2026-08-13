#include "dns_server.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "config_store.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "wifi_link.h"

static const char *TAG = "dns_server";

#define DNS_PORT 53
#define DNS_BUF 512
#define SPOOF_SUFFIX "wunderground.com"
#define SPOOF_TTL 60

// OS connectivity-check ("is there internet?") hostnames. Spoofing these to the
// AP IP - even once provisioned - makes Android/iOS/Windows show their captive
// "sign in" UI and open the setup page. The weather station never queries them,
// so its DNS is unaffected. www.google.com is Android's HTTPS validation probe:
// pointing it at us (where no TLS listens) makes that probe fail, which is what
// forces Android to show the portal instead of silently validating.
static bool is_captive_probe(const char *name) {
  static const char *const hosts[] = {
      "connectivitycheck.gstatic.com",
      "connectivitycheck.android.com",
      "clients3.google.com",
      "www.google.com",
      "captive.apple.com",
      "msftconnecttest.com",
      "www.msftconnecttest.com",
      "msftncsi.com",
      "www.msftncsi.com",
      "detectportal.firefox.com",
  };
  for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
    if (strcmp(name, hosts[i]) == 0) {
      return true;
    }
  }
  return false;
}

// Parse the single question name. Returns 1 if it should be answered with our
// AP IP (the WU spoof suffix or an OS captive-probe host), 0 if not, -1 on a
// packet we won't synthesize for (proxy it instead). On success *q_end is the
// offset just past QNAME (QTYPE follows) and the lowercased name is copied to
// name_out (name_cap incl. NUL) for logging.
static int classify_query(const uint8_t *msg, int len, int *q_end,
                          char *name_out, size_t name_cap) {
  if (len < 12) {
    return -1;
  }
  int qdcount = (msg[4] << 8) | msg[5];
  if (qdcount != 1) {
    return -1;
  }
  char name[256];
  int ni = 0;
  int i = 12;
  while (i < len) {
    uint8_t label_len = msg[i++];
    if (label_len == 0) {
      break;
    }
    if (label_len & 0xC0) {
      return -1;  // compression pointer in a question: bail to proxy
    }
    for (int k = 0; k < label_len; k++) {
      if (i >= len) {
        return -1;
      }
      char c = (char)tolower((unsigned char)msg[i++]);
      if (ni < 255) {
        name[ni++] = c;
      }
    }
    if (ni < 255) {
      name[ni++] = '.';
    }
  }
  if (ni > 0 && name[ni - 1] == '.') {
    ni--;
  }
  name[ni] = '\0';
  *q_end = i;
  snprintf(name_out, name_cap, "%s", name);

  size_t nl = strlen(name);
  size_t sl = strlen(SPOOF_SUFFIX);
  bool wu = (nl >= sl && strcmp(name + nl - sl, SPOOF_SUFFIX) == 0);
  return (wu || is_captive_probe(name)) ? 1 : 0;
}

// Build an A-record answer pointing the queried name at ap_addr (raw 4 bytes
// a.b.c.d as stored in esp_ip4_addr_t). Returns response length or -1.
static int build_a_response(const uint8_t *query, int qlen, int q_end,
                            uint32_t ap_addr, uint8_t *out, int outcap) {
  int qsec_end = q_end + 4;  // + QTYPE(2) + QCLASS(2)
  if (qsec_end > qlen || qsec_end + 16 > outcap) {
    return -1;
  }
  memcpy(out, query, qsec_end);
  out[2] = 0x81;  // QR=1, RD copied as 1
  out[3] = 0x80;  // RA=1
  out[6] = 0x00;
  out[7] = 0x01;                               // ANCOUNT = 1
  out[8] = out[9] = out[10] = out[11] = 0x00;  // NSCOUNT / ARCOUNT = 0
  int p = qsec_end;
  out[p++] = 0xC0;
  out[p++] = 0x0C;  // pointer to QNAME at offset 12
  out[p++] = 0x00;
  out[p++] = 0x01;  // TYPE A
  out[p++] = 0x00;
  out[p++] = 0x01;  // CLASS IN
  out[p++] = 0x00;
  out[p++] = 0x00;
  out[p++] = 0x00;
  out[p++] = SPOOF_TTL;  // TTL
  out[p++] = 0x00;
  out[p++] = 0x04;  // RDLENGTH = 4
  memcpy(out + p, &ap_addr, 4);
  p += 4;
  return p;
}

// Build a NODATA response (QR=1, 0 answers) echoing the question. Used for
// non-A queries (e.g. AAAA/IPv6) while spoofing, so resolvers get a well-formed
// "no such record type" instead of an A record sent for the wrong qtype.
static int build_empty_response(const uint8_t *query, int qlen, int q_end,
                                uint8_t *out, int outcap) {
  int qsec_end = q_end + 4;  // + QTYPE(2) + QCLASS(2)
  if (qsec_end > qlen || qsec_end > outcap) {
    return -1;
  }
  memcpy(out, query, qsec_end);
  out[2] = 0x81;             // QR=1, RD copied
  out[3] = 0x80;             // RA=1
  out[6] = out[7] = 0x00;    // ANCOUNT = 0
  out[8] = out[9] = 0x00;    // NSCOUNT = 0
  out[10] = out[11] = 0x00;  // ARCOUNT = 0
  return qsec_end;
}

static uint32_t ap_ip_addr(void) {
  esp_netif_ip_info_t ip = {0};
  esp_netif_get_ip_info(wifi_link_ap_netif(), &ip);
  return ip.ip.addr;
}

static uint32_t upstream_dns_addr(void) {
  esp_netif_dns_info_t dns = {0};
  if (esp_netif_get_dns_info(wifi_link_sta_netif(), ESP_NETIF_DNS_MAIN, &dns) ==
          ESP_OK &&
      dns.ip.u_addr.ip4.addr != 0) {
    return dns.ip.u_addr.ip4.addr;
  }
  return ipaddr_addr("8.8.8.8");  // fallback if STA DNS not yet known
}

static void proxy_upstream(int client_sock, const struct sockaddr_in *client,
                           const uint8_t *query, int qlen) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) {
    return;
  }
  struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct sockaddr_in up = {
      .sin_family = AF_INET,
      .sin_port = htons(DNS_PORT),
      .sin_addr.s_addr = upstream_dns_addr(),
  };
  if (sendto(s, query, qlen, 0, (struct sockaddr *)&up, sizeof(up)) == qlen) {
    uint8_t resp[DNS_BUF];
    int r = recvfrom(s, resp, sizeof(resp), 0, NULL, NULL);
    if (r > 0) {
      sendto(client_sock, resp, r, 0, (const struct sockaddr *)client,
             sizeof(*client));
    }
  }
  close(s);
}

static void dns_task(void *arg) {
  (void)arg;
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    ESP_LOGE(TAG, "socket failed");
    vTaskDelete(NULL);
    return;
  }
  struct sockaddr_in bind_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(DNS_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    ESP_LOGE(TAG, "bind :53 failed");
    close(sock);
    vTaskDelete(NULL);
    return;
  }
  ESP_LOGI(TAG, "DNS server up: spoofing *.%s", SPOOF_SUFFIX);

  uint8_t query[DNS_BUF];
  while (true) {
    struct sockaddr_in client = {0};
    socklen_t client_len = sizeof(client);
    int qlen = recvfrom(sock, query, sizeof(query), 0,
                        (struct sockaddr *)&client, &client_len);
    if (qlen <= 0) {
      continue;
    }
    int q_end = 0;
    char qname[256] = "";
    int verdict = classify_query(query, qlen, &q_end, qname, sizeof(qname));
    ESP_LOGI(TAG, "query \"%s\" from " IPSTR " -> %s", qname,
             IP2STR((esp_ip4_addr_t *)&client.sin_addr.s_addr),
             verdict == 1 ? "spoof" : (verdict == 0 ? "proxy" : "drop"));
    // While unconfigured, run as a captive portal: answer every lookup with our
    // AP IP so the phone's captive-portal probe lands on the config page. Once
    // configured, only WU is spoofed and the rest is proxied (the station needs
    // working DNS). The STA uplink is down while unconfigured anyway, so there
    // is nothing useful to proxy to.
    bool captive = !config_is_provisioned();
    if (verdict == 1 || (captive && verdict == 0)) {
      // qtype follows the QNAME. Only answer A (1) with our IP; reply NODATA to
      // AAAA/others so resolvers don't choke on a type-mismatched answer.
      uint16_t qtype = (q_end + 1 < qlen)
                           ? (uint16_t)((query[q_end] << 8) | query[q_end + 1])
                           : 0;
      uint8_t resp[DNS_BUF];
      int rlen =
          (qtype == 1)
              ? build_a_response(query, qlen, q_end, ap_ip_addr(), resp,
                                 sizeof(resp))
              : build_empty_response(query, qlen, q_end, resp, sizeof(resp));
      if (rlen > 0) {
        sendto(sock, resp, rlen, 0, (struct sockaddr *)&client, client_len);
      }
    } else if (verdict == 0) {
      proxy_upstream(sock, &client, query, qlen);
    }
  }
}

void dns_server_start(void) {
  xTaskCreate(dns_task, "dns_server", 4096, NULL, 5, NULL);
}
