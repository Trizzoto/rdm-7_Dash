/* web_server_captive.c — captive portal probe handlers
 *
 * Each phone OS probes a well-known URL on join to detect "real" internet.
 * If we don't respond captive-portal-style, modern Android (10+) and iOS
 * silently demote the WiFi to "limited" and the editor becomes unreachable.
 * So the probe handlers always 302 the OS straight into the editor at the
 * dash root. The captive-portal sign-in sheet renders the editor directly
 * — no intermediate welcome page.
 *
 * The redirect target uses a hardcoded RDM-branded hostname rather than the
 * Host header from the probe, so the captive-portal mini-browser shows
 * "rdm-7-dash" in the address bar instead of "connectivitycheck.gstatic.com"
 * or "captive.apple.com".
 *
 * Each platform's probe URL is a separate httpd_uri_t entry — do NOT merge
 * them into a wildcard handler. */
#include "web_server_internal.h"
#include <string.h>

static const char *TAG = "web_server_captive";

/* RDM-branded hostname shown in the captive-portal address bar. */
#define RDM_REDIRECT_HOST "rdm-7-dash"

static esp_err_t captive_portal_redirect_handler(httpd_req_t *req) {
	ESP_LOGI(TAG, "Captive portal probe: %s", req->uri);
	httpd_resp_set_status(req, "302 Found");
	httpd_resp_set_hdr(req, "Location", "http://" RDM_REDIRECT_HOST "/");
	httpd_resp_set_type(req, "text/html; charset=UTF-8");
	/* Non-empty body required for iOS to recognise this as a captive portal
	 * (some versions ignore bare 302). The OS follows the 302 before
	 * rendering, so this body is rarely shown. */
	const char *body =
		"<!doctype html><html><head>"
		"<meta http-equiv=\"refresh\" content=\"0;url=http://" RDM_REDIRECT_HOST "/\">"
		"<title>RDM-7 Dash</title></head>"
		"<body>Opening dash editor&hellip;</body></html>";
	return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t cp_ios_uri = {
	.uri = "/hotspot-detect.html", .method = HTTP_GET,
	.handler = captive_portal_redirect_handler, .user_ctx = NULL};
static const httpd_uri_t cp_ios2_uri = {
	.uri = "/library/test/success.html", .method = HTTP_GET,
	.handler = captive_portal_redirect_handler, .user_ctx = NULL};
static const httpd_uri_t cp_android_uri = {
	.uri = "/generate_204", .method = HTTP_GET,
	.handler = captive_portal_redirect_handler, .user_ctx = NULL};
static const httpd_uri_t cp_android2_uri = {
	.uri = "/gen_204", .method = HTTP_GET,
	.handler = captive_portal_redirect_handler, .user_ctx = NULL};
static const httpd_uri_t cp_android3_uri = {
	.uri = "/generate204", .method = HTTP_GET,
	.handler = captive_portal_redirect_handler, .user_ctx = NULL};
static const httpd_uri_t cp_win_uri = {
	.uri = "/connecttest.txt", .method = HTTP_GET,
	.handler = captive_portal_redirect_handler, .user_ctx = NULL};
static const httpd_uri_t cp_win2_uri = {
	.uri = "/ncsi.txt", .method = HTTP_GET,
	.handler = captive_portal_redirect_handler, .user_ctx = NULL};
static const httpd_uri_t cp_win3_uri = {
	.uri = "/redirect", .method = HTTP_GET,
	.handler = captive_portal_redirect_handler, .user_ctx = NULL};
static const httpd_uri_t cp_firefox_uri = {
	.uri = "/success.txt", .method = HTTP_GET,
	.handler = captive_portal_redirect_handler, .user_ctx = NULL};

void web_server_captive_register(httpd_handle_t server) {
	REGISTER_URI(server, &cp_ios_uri);
	REGISTER_URI(server, &cp_ios2_uri);
	REGISTER_URI(server, &cp_android_uri);
	REGISTER_URI(server, &cp_android2_uri);
	REGISTER_URI(server, &cp_android3_uri);
	REGISTER_URI(server, &cp_win_uri);
	REGISTER_URI(server, &cp_win2_uri);
	REGISTER_URI(server, &cp_win3_uri);
	REGISTER_URI(server, &cp_firefox_uri);
}
