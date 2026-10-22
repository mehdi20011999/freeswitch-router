// sip_parse.c - see include/sip/sip_parse.h
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // strcasestr
#endif
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "sip/sip_parse.h"

static bool copy_trimmed(const char *start, const char *end, char *out, size_t outlen) {
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    size_t len = (size_t)(end - start);
    if (outlen == 0 || len >= outlen) return false;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

bool sip_method(const char *msg, char *out, size_t outlen) {
    if (!msg || !out || outlen == 0) return false;
    size_t n = 0;
    while (msg[n] && isupper((unsigned char)msg[n])) n++;
    if (n == 0 || n >= outlen || msg[n] != ' ') return false;
    memcpy(out, msg, n);
    out[n] = '\0';
    return true;
}

static const char *compact_form(const char *name) {
    if (strcasecmp(name, "Call-ID") == 0) return "i";
    if (strcasecmp(name, "From") == 0) return "f";
    if (strcasecmp(name, "To") == 0) return "t";
    if (strcasecmp(name, "Via") == 0) return "v";
    if (strcasecmp(name, "Contact") == 0) return "m";
    return NULL;
}

// Find the start of the next line, or NULL at the end of the header block.
static const char *next_line(const char *p) {
    const char *nl = strchr(p, '\n');
    if (!nl) return NULL;
    nl++;
    if (*nl == '\r' || *nl == '\n' || *nl == '\0') return NULL;  // blank line: body follows
    return nl;
}

static bool line_has_header(const char *line, const char *name, const char **value) {
    size_t len = strlen(name);
    if (strncasecmp(line, name, len) != 0) return false;
    const char *p = line + len;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return false;
    *value = p + 1;
    return true;
}

bool sip_header(const char *msg, const char *name, char *out, size_t outlen) {
    if (!msg || !name || !out || outlen == 0) return false;
    out[0] = '\0';
    const char *compact = compact_form(name);
    for (const char *line = next_line(msg); line; line = next_line(line)) {
        const char *value = NULL;
        if (line_has_header(line, name, &value) || (compact && line_has_header(line, compact, &value))) {
            const char *end = value;
            while (*end && *end != '\r' && *end != '\n') end++;
            return copy_trimmed(value, end, out, outlen);
        }
    }
    return false;
}

bool sip_uri_user(const char *value, char *out, size_t outlen) {
    if (!value || !out || outlen == 0) return false;
    out[0] = '\0';
    const char *p = NULL;
    const char *schemes[] = {"sip:", "sips:", "tel:"};
    for (size_t i = 0; i < 3; i++) {
        const char *hit = strcasestr(value, schemes[i]);
        if (hit && (!p || hit < p)) p = hit + strlen(schemes[i]);
    }
    if (!p) return false;
    const char *end = p;
    while (*end && *end != '@' && *end != '>' && *end != ';' && *end != ' ' && *end != ':') end++;
    if (end == p) return false;
    return copy_trimmed(p, end, out, outlen);
}

__attribute__((format(printf, 4, 5)))
static bool appendf(char *out, size_t outlen, size_t *used, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *used, outlen - *used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= outlen - *used) return false;
    *used += (size_t)n;
    return true;
}

int sip_build_response(char *out, size_t outlen, const char *request,
                       int code, const char *reason, const char *contact) {
    if (!out || outlen == 0 || !request || !reason) return -1;
    char via[1024], from[512] = "", to[512] = "", callid[256], cseq[128];
    // Via, Call-ID and CSeq let the client match the response; without them
    // no useful answer exists. From/To are copied when present, so a request
    // missing one of them can still get its 400.
    if (!sip_header(request, "Via", via, sizeof(via)) ||
        !sip_header(request, "Call-ID", callid, sizeof(callid)) ||
        !sip_header(request, "CSeq", cseq, sizeof(cseq))) {
        return -1;
    }
    sip_header(request, "From", from, sizeof(from));
    sip_header(request, "To", to, sizeof(to));
    // A UAS adds a tag to To on every response but 100 (RFC 3261 8.2.6.2).
    char to_tagged[600];
    if (to[0] && code != 100 && !strcasestr(to, ";tag=")) {
        snprintf(to_tagged, sizeof(to_tagged), "%s;tag=s2r%08x", to, (unsigned)(strlen(callid) * 2654435761u));
    } else {
        snprintf(to_tagged, sizeof(to_tagged), "%s", to);
    }

    size_t used = 0;
    if (!appendf(out, outlen, &used, "SIP/2.0 %d %s\r\nVia: %s\r\n", code, reason, via) ||
        (from[0] && !appendf(out, outlen, &used, "From: %s\r\n", from)) ||
        (to[0] && !appendf(out, outlen, &used, "To: %s\r\n", to_tagged)) ||
        !appendf(out, outlen, &used, "Call-ID: %s\r\nCSeq: %s\r\n", callid, cseq) ||
        (contact && !appendf(out, outlen, &used, "Contact: <%s>\r\n", contact)) ||
        !appendf(out, outlen, &used, "Server: freeswitch-router\r\nContent-Length: 0\r\n\r\n")) {
        return -1;
    }
    return (int)used;
}
