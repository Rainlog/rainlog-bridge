#include "session_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_store.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_util.h"

// A handful of concurrent signed-in browsers is plenty for a home device; a
// new login past that evicts the oldest session (round-robin).
#define SESSION_MAX 4
#define TOKEN_HEX_LEN 32  // 16 random bytes as hex
#define COOKIE_NAME "rlb_session"

#define LOGIN_PAGE_MAX 2048

static char s_sessions[SESSION_MAX][TOKEN_HEX_LEN + 1];
static uint8_t s_next_slot;

// The sign-in page, styled to match the embedded config page (web/src). The
// Format arguments are the allowlisted return route and inline error line.
static const char LOGIN_PAGE_FMT[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Rainlog Bridge</title><style>"
    "body{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;"
    "background:#eef2f7;color:#222;margin:0;padding:3em .8em;font-size:17px}"
    ".card{max-width:380px;margin:0 auto;background:#fff;border-radius:16px;"
    "padding:1.4em 1.4em 2em;box-shadow:0 2px 12px rgba(20,40,80,.1)}"
    "h2{margin:0 0 .4em;font-size:1.6em}"
    ".hint{margin:.3em 0 .7em;color:#667;font-size:.9em}"
    "label{display:block;margin:1em 0 .35em;font-weight:600}"
    "input{display:block;width:100%%;min-height:52px;padding:.7em .8em;"
    "border:1px solid #c6cfd9;border-radius:10px;box-sizing:border-box;"
    "font-size:1.05em;background:#fafcfe}"
    "input:focus{outline:none;border-color:#1668c8;"
    "box-shadow:0 0 0 3px rgba(22,104,200,.18)}"
    ".err{min-height:1.2em;margin:.5em 0 0;color:#c0392b;font-size:.9em}"
    "button{display:block;width:100%%;margin-top:1em;padding:1em;"
    "min-height:52px;font-size:1.15em;background:#1668c8;color:#fff;"
    "font-weight:600;border:none;border-radius:10px;cursor:pointer}"
    "</style></head><body><form class=card method=POST action=/login?next=%s>"
    "<h2>Rainlog Bridge</h2>"
    "<p class=hint>Enter the bridge&rsquo;s Wi-Fi password to open the bridge.</p>"
    "<label for=pw>Password</label>"
    "<input id=pw name=password type=password autofocus "
    "autocomplete=current-password>"
    "<div class=err>%s</div>"
    "<button type=submit>Sign in</button>"
    "</form></body></html>";

// Constant-time string equality: every byte of the shorter string is compared
// regardless of where the first mismatch is.
static bool ct_str_eq(const char *a, const char *b) {
  size_t alen = strlen(a);
  size_t blen = strlen(b);
  unsigned diff = (unsigned)(alen ^ blen);
  for (size_t i = 0; i < alen && i < blen; i++) {
    diff |= (unsigned)(a[i] ^ b[i]);
  }
  return diff == 0;
}

bool session_auth_password_matches(const char *given) {
  return ct_str_eq(given, config_get()->ap_pass);
}

bool session_auth_ok(httpd_req_t *req) {
  char tok[TOKEN_HEX_LEN + 1];
  size_t len = sizeof(tok);
  if (httpd_req_get_cookie_val(req, COOKIE_NAME, tok, &len) != ESP_OK) {
    return false;
  }
  for (int i = 0; i < SESSION_MAX; i++) {
    if (s_sessions[i][0] != '\0' && ct_str_eq(tok, s_sessions[i])) {
      return true;
    }
  }
  return false;
}

// Return only known app routes, never a user-supplied redirect destination.
static const char *login_return_route(httpd_req_t *req) {
  char query[64], next[32] = "";
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    http_util_form_get(query, "next", next, sizeof(next));
  const char *const routes[] = {"/setup", "/devices", "/firmware"};
  for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
    size_t length = strlen(routes[i]);
    if (!strcmp(next, routes[i]) ||
        (!strncmp(req->uri, routes[i], length) &&
         (req->uri[length] == '\0' || req->uri[length] == '?')))
      return routes[i];
  }
  return "/setup";
}

esp_err_t session_auth_send_login_page(httpd_req_t *req, bool wrong_password) {
  char *page = malloc(LOGIN_PAGE_MAX);
  if (page == NULL) {
    return ESP_ERR_NO_MEM;
  }
  snprintf(page, LOGIN_PAGE_MAX, LOGIN_PAGE_FMT, login_return_route(req),
           wrong_password ? "Wrong password." : "");
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_type(req, "text/html");
  esp_err_t err = httpd_resp_sendstr(req, page);
  free(page);
  return err;
}

esp_err_t session_auth_login_handler(httpd_req_t *req) {
  char *body = http_util_read_body(req, 512);
  char pass[65] = "";
  if (body != NULL) {
    http_util_form_get(body, "password", pass, sizeof(pass));
    free(body);
  }
  if (!session_auth_password_matches(pass)) {
    // Throttle online guessing of the bridge password. Blocking the (shared)
    // httpd task is acceptable: only a failed login pays the second, and a
    // console upload queued behind it is answered right after.
    vTaskDelay(pdMS_TO_TICKS(1000));
    return session_auth_send_login_page(req, true);
  }

  char *tok = s_sessions[s_next_slot];
  s_next_slot = (uint8_t)((s_next_slot + 1) % SESSION_MAX);
  uint8_t raw[TOKEN_HEX_LEN / 2];
  esp_fill_random(raw, sizeof(raw));
  for (size_t i = 0; i < sizeof(raw); i++) {
    snprintf(&tok[i * 2], 3, "%02x", raw[i]);
  }

  // Max-Age keeps the cookie across browser restarts; the real session
  // lifetime is until the bridge reboots (RAM store) or the slot is evicted.
  char cookie[128];
  snprintf(cookie, sizeof(cookie),
           COOKIE_NAME
           "=%s; Path=/; Max-Age=31536000; HttpOnly; "
           "SameSite=Strict",
           tok);
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Set-Cookie", cookie);
  httpd_resp_set_hdr(req, "Location", login_return_route(req));
  return httpd_resp_send(req, NULL, 0);
}

esp_err_t session_auth_logout_handler(httpd_req_t *req) {
  char tok[TOKEN_HEX_LEN + 1];
  size_t len = sizeof(tok);
  if (httpd_req_get_cookie_val(req, COOKIE_NAME, tok, &len) == ESP_OK) {
    for (int i = 0; i < SESSION_MAX; i++) {
      if (s_sessions[i][0] != '\0' && ct_str_eq(tok, s_sessions[i])) {
        s_sessions[i][0] = '\0';
      }
    }
  }
  httpd_resp_set_hdr(req, "Set-Cookie",
                     COOKIE_NAME
                     "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"s\":\"ok\"}");
}
