/* web_server_captive.c — captive portal probe handlers
 *
 * When a phone joins the dash hotspot its OS probes well-known URLs to detect
 * internet connectivity.  Without a response iOS backgrounds the connection
 * (preferring cellular) and Android shows "No internet", making the dash
 * unreachable even though the AP is up.
 *
 * By responding with a 302 redirect we trigger the OS "Sign in to network"
 * captive-portal sheet, which loads the dash web editor in a mini-browser.
 * iOS, Android 5+, and Windows all recognise this pattern.
 *
 * Each platform's probe URL is a separate httpd_uri_t entry — do NOT merge
 * them into a wildcard handler.  See ADR 0001 for the reasoning. */
#include "web_server_internal.h"

static const char *TAG = "web_server_captive";

static esp_err_t captive_portal_redirect_handler(httpd_req_t *req) {
	ESP_LOGI(TAG, "Captive portal probe: %s", req->uri);
	/* Build Location from the Host header so we redirect to whichever IP
	 * the client used (192.168.4.1 on AP, STA IP on LAN). Falls back to
	 * the AP IP if Host is unavailable. */
	char host[48] = "192.168.4.1";
	size_t h_len = httpd_req_get_hdr_value_len(req, "Host");
	if (h_len > 0 && h_len < sizeof(host)) {
		httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
	}
	char loc[96];
	snprintf(loc, sizeof(loc), "http://%s/", host);
	httpd_resp_set_status(req, "302 Found");
	httpd_resp_set_hdr(req, "Location", loc);
	httpd_resp_set_type(req, "text/html; charset=UTF-8");
	/* Non-empty body is required for iOS to treat this as a real
	 * captive portal (bare 302 is ignored on some versions). */
	const char *body =
		"<!doctype html><html><head>"
		"<meta http-equiv=\"refresh\" content=\"0;url=http://192.168.4.1/\">"
		"<title>RDM-7 Dash</title></head>"
		"<body>Redirecting to dash editor&hellip;</body></html>";
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
