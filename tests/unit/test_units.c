// Unit tests for the pure modules: SIP parsing and input safety.
// Built with AddressSanitizer + UndefinedBehaviorSanitizer by `make test`.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/safety.h"
#include "sip/sip_parse.h"

static int failures = 0, checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        checks++;                                                         \
        if (!(cond)) {                                                    \
            failures++;                                                   \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

#define CHECK_STR(a, b) CHECK(strcmp((a), (b)) == 0)

static const char *INVITE =
    "INVITE sip:12125550000@10.0.0.2 SIP/2.0\r\n"
    "Via: SIP/2.0/UDP 10.0.0.1:5060;branch=z9hG4bK776asdhds\r\n"
    "Max-Forwards: 70\r\n"
    "To: <sip:12125550000@10.0.0.2>\r\n"
    "From: \"Alice\" <sip:+15550001111@10.0.0.1>;tag=1928301774\r\n"
    "Call-ID: a84b4c76e66710@pc33.example.com\r\n"
    "CSeq: 314159 INVITE\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

static void test_headers_do_not_alias(void) {
    // The original parser returned one static buffer for every header.
    char call_id[128], from[128], to[128];
    CHECK(sip_header(INVITE, "Call-ID", call_id, sizeof(call_id)));
    CHECK(sip_header(INVITE, "From", from, sizeof(from)));
    CHECK(sip_header(INVITE, "To", to, sizeof(to)));
    CHECK_STR(call_id, "a84b4c76e66710@pc33.example.com");
    CHECK_STR(from, "\"Alice\" <sip:+15550001111@10.0.0.1>;tag=1928301774");
    CHECK_STR(to, "<sip:12125550000@10.0.0.2>");
}

static void test_header_matching(void) {
    char v[64];
    const char *compact = "INVITE sip:1@x SIP/2.0\r\ni: abc\r\nf: <sip:1@x>\r\nt: <sip:2@x>\r\n\r\n";
    CHECK(sip_header(compact, "Call-ID", v, sizeof(v)));
    CHECK_STR(v, "abc");
    CHECK(sip_header(compact, "to", v, sizeof(v)));
    CHECK_STR(v, "<sip:2@x>");
    // "To" must not match "Tomato:" and the body is never searched.
    const char *tricky = "INVITE sip:1@x SIP/2.0\r\nTomato: red\r\n\r\nTo: <sip:9@evil>\r\n";
    CHECK(!sip_header(tricky, "To", v, sizeof(v)));
    // A value longer than the buffer is refused, not truncated.
    CHECK(!sip_header(INVITE, "Via", v, 10));
    CHECK(!sip_header(NULL, "To", v, sizeof(v)));
}

static void test_uri_user(void) {
    char u[64];
    CHECK(sip_uri_user("\"Alice\" <sip:+15550001111@10.0.0.1>;tag=1", u, sizeof(u)));
    CHECK_STR(u, "+15550001111");
    CHECK(sip_uri_user("<sips:12125550000@host:5061>", u, sizeof(u)));
    CHECK_STR(u, "12125550000");
    CHECK(sip_uri_user("<tel:+212600000000>", u, sizeof(u)));
    CHECK_STR(u, "+212600000000");
    CHECK(!sip_uri_user("<mailto:a@b>", u, sizeof(u)));
    CHECK(!sip_uri_user("<sip:@host>", u, sizeof(u)));
}

static void test_method(void) {
    char m[16];
    CHECK(sip_method(INVITE, m, sizeof(m)));
    CHECK_STR(m, "INVITE");
    CHECK(!sip_method("SIP/2.0 200 OK\r\n", m, sizeof(m)));  // a response, not a request
    CHECK(!sip_method("", m, sizeof(m)));
    CHECK(!sip_method("invite sip:x SIP/2.0", m, sizeof(m)));
}

static void test_response(void) {
    char out[2048];
    int n = sip_build_response(out, sizeof(out), INVITE, 302, "Moved Temporarily",
                               "sip:12125551001@10.0.0.3:5060");
    CHECK(n > 0 && (size_t)n == strlen(out));
    CHECK(strncmp(out, "SIP/2.0 302 Moved Temporarily\r\n", 31) == 0);
    CHECK(strstr(out, "Via: SIP/2.0/UDP 10.0.0.1:5060;branch=z9hG4bK776asdhds\r\n") != NULL);
    CHECK(strstr(out, "CSeq: 314159 INVITE\r\n") != NULL);
    CHECK(strstr(out, "Call-ID: a84b4c76e66710@pc33.example.com\r\n") != NULL);
    CHECK(strstr(out, "Contact: <sip:12125551001@10.0.0.3:5060>\r\n") != NULL);
    CHECK(strstr(out, "To: <sip:12125550000@10.0.0.2>;tag=") != NULL);  // UAS tag added
    CHECK(strstr(out, "\r\n\r\n") == out + n - 4);

    n = sip_build_response(out, sizeof(out), INVITE, 100, "Trying", NULL);
    CHECK(n > 0 && strstr(out, "To: <sip:12125550000@10.0.0.2>\r\n") != NULL);  // no tag on 100

    // A request without From still gets an answer (400), without a From line.
    const char *no_from = "INVITE sip:1@x SIP/2.0\r\nVia: SIP/2.0/UDP h;branch=z9hG4bK1\r\n"
                          "To: <sip:1@x>\r\nCall-ID: c1\r\nCSeq: 1 INVITE\r\n\r\n";
    n = sip_build_response(out, sizeof(out), no_from, 400, "Bad Request", NULL);
    CHECK(n > 0 && strstr(out, "From:") == NULL && strstr(out, "Call-ID: c1\r\n") != NULL);

    // No Via/Call-ID/CSeq, or no room: refused.
    CHECK(sip_build_response(out, sizeof(out), "INVITE sip:x SIP/2.0\r\nTo: <sip:1@x>\r\n\r\n", 400, "Bad", NULL) < 0);
    CHECK(sip_build_response(out, 40, INVITE, 200, "OK", NULL) < 0);
}

static void test_numbers_and_tokens(void) {
    CHECK(safe_is_number("12125550000"));
    CHECK(safe_is_number("+212600000000"));
    CHECK(!safe_is_number(""));
    CHECK(!safe_is_number("+"));
    CHECK(!safe_is_number("1'; DROP TABLE dids;--"));
    CHECK(!safe_is_number("123456789012345678901234567890123"));  // 33 digits
    CHECK(safe_is_token("a1b2c3d4-0001-4000-8000-000000000001"));
    CHECK(!safe_is_token("x;rm -rf /"));
    CHECK(!safe_is_token("abc$(id)"));
    CHECK(safe_is_host("10.0.0.1") && safe_is_host("sip.example.com") && safe_is_host("2001:db8::1"));
    CHECK(!safe_is_host("10.0.0.1\"/><x"));
}

static void test_patterns(void) {
    CHECK(safe_is_route_pattern("1[2-9][0-9]{9}"));
    CHECK(safe_is_route_pattern("212[5-7]\\d{8}"));
    CHECK(!safe_is_route_pattern("1[2-9"));          // does not compile
    CHECK(!safe_is_route_pattern("1\"/><x a=\""));   // would break the XML attribute
    CHECK(!safe_is_route_pattern(""));
    CHECK(safe_pattern_matches("1[2-9][0-9]{9}", "12125550000"));
    CHECK(!safe_pattern_matches("1[2-9][0-9]{9}", "112125550000"));  // anchored at both ends
    CHECK(!safe_pattern_matches("1[2-9][0-9]{9}", "1212555000"));
    CHECK(safe_pattern_matches("212[5-7]\\d{8}", "212612345678"));
    CHECK(safe_pattern_matches("33|44", "44"));
    CHECK(!safe_pattern_matches("33|44", "344"));  // alternation stays inside the anchors
}

static void test_ip_list(void) {
    CHECK(safe_ip_in_list("10.0.0.1", "10.0.0.1"));
    CHECK(safe_ip_in_list("10.0.0.5, 10.0.0.1;10.0.0.9", "10.0.0.1"));
    CHECK(!safe_ip_in_list("10.0.0.10", "10.0.0.1"));  // strstr() accepted this
    CHECK(!safe_ip_in_list("10.0.0.1", "10.0.0.10"));
    CHECK(!safe_ip_in_list("", "10.0.0.1"));
    CHECK(!safe_ip_in_list("10.0.0.1", ""));
}

static void test_xml_escape(void) {
    char out[64];
    CHECK(safe_xml_escape("a<b>&\"c'", out, sizeof(out)));
    CHECK_STR(out, "a&lt;b&gt;&amp;&quot;c&apos;");
    CHECK(!safe_xml_escape("&&&&&&&&&&", out, 20));
    CHECK_STR(out, "");
}

static void test_run(void) {
    const char *ok[] = {"true", NULL};
    const char *bad[] = {"false", NULL};
    const char *missing[] = {"this-program-does-not-exist-42", NULL};
    CHECK(safe_run(ok) == 0);
    CHECK(safe_run(bad) == 1);
    CHECK(safe_run(missing) != 0);
    // Arguments are not interpreted by a shell.
    const char *echo[] = {"sh", "-c", "test \"$1\" = 'a;b $(x)'", "sh", "a;b $(x)", NULL};
    CHECK(safe_run(echo) == 0);
}

// Random bytes must never crash the parser (ASan/UBSan watch the reads).
static void test_fuzz(void) {
    char msg[512], out[256];
    unsigned seed = 12345;
    for (int round = 0; round < 20000; round++) {
        size_t len = (size_t)(rand_r(&seed) % (sizeof(msg) - 1));
        for (size_t i = 0; i < len; i++) {
            const char alphabet[] = "INVITE SIP/2.0\r\n:;<>@ToFromCall-IDCSeqVia sip:tel:0123456789";
            msg[i] = alphabet[rand_r(&seed) % (sizeof(alphabet) - 1)];
        }
        msg[len] = '\0';
        char m[16], v[64], u[32];
        sip_method(msg, m, sizeof(m));
        if (sip_header(msg, "From", v, sizeof(v))) sip_uri_user(v, u, sizeof(u));
        sip_header(msg, "i", v, sizeof(v));
        sip_build_response(out, sizeof(out), msg, 404, "Not Found", "sip:1@x");
    }
    CHECK(1);
}

int main(void) {
    test_headers_do_not_alias();
    test_header_matching();
    test_uri_user();
    test_method();
    test_response();
    test_numbers_and_tokens();
    test_patterns();
    test_ip_list();
    test_xml_escape();
    test_run();
    test_fuzz();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
