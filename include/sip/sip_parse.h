// sip_parse.h - minimal, allocation-free SIP message helpers.
//
// All functions write into caller-provided buffers. The original server kept
// header values in one static buffer, so reading From after Call-ID silently
// overwrote the Call-ID (and From/To/Call-ID all ended up equal).
#ifndef SIP_PARSE_H
#define SIP_PARSE_H

#include <stdbool.h>
#include <stddef.h>

// Request method ("INVITE", ...) into out. Returns false on a malformed start line.
bool sip_method(const char *msg, char *out, size_t outlen);

// Value of header `name` (case-insensitive, compact forms i/f/t/v/m accepted),
// leading/trailing spaces removed. Only the header block is searched.
bool sip_header(const char *msg, const char *name, char *out, size_t outlen);

// User part of the first sip:/sips:/tel: URI in a From/To/Contact value:
//   "Alice" <sip:+15551234@10.0.0.1>;tag=x  ->  +15551234
bool sip_uri_user(const char *value, char *out, size_t outlen);

// Build a response to `request` (RFC 3261 8.2.6: Via, From, To, Call-ID and
// CSeq copied from the request; From/To only if present). `contact` may be NULL; for 3xx it is the
// redirect target, e.g. "sip:12125551001@10.0.0.3:5060".
// Returns the length written, or -1 if the request lacks Via, Call-ID or CSeq
// or the buffer is too small.
int sip_build_response(char *out, size_t outlen, const char *request,
                       int code, const char *reason, const char *contact);

#endif
