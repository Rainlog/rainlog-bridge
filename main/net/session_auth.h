// Rainlog Wireless Bridge - LAN-side sign-in sessions for the config page.
//
// The SoftAP side stays open (joining the bridge's WiFi already required the
// password, and captive-portal webviews can't show auth prompts). From the
// home LAN the same secret - the bridge WiFi password - is entered on a real
// login form instead of an HTTP Basic auth popup; a random session cookie
// then keeps the browser signed in. Sessions live in RAM only: a reboot
// signs everyone out.
#pragma once

#include <stdbool.h>

#include "esp_http_server.h"

// True if `given` equals the configured bridge WiFi password. Constant-time
// compare: doesn't leak how much of the password matched. Shared by the login
// form and the Basic auth fallback (curl / scripts).
bool session_auth_password_matches(const char *given);

// True if the request carries a session cookie issued by a successful login.
bool session_auth_ok(httpd_req_t *req);

// Serve the sign-in page (sent with status 401; the form POSTs to /login).
// wrong_password shows the inline "wrong password" error.
esp_err_t session_auth_send_login_page(httpd_req_t *req, bool wrong_password);

// POST /login: check form field `password`; on success set the session cookie
// and redirect to /. On failure re-serve the login page (throttled).
esp_err_t session_auth_login_handler(httpd_req_t *req);

// POST /logout: drop the request's session and expire its cookie.
esp_err_t session_auth_logout_handler(httpd_req_t *req);
