// safety.c - see include/core/safety.h
#include <ctype.h>
#include <regex.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include "core/safety.h"

extern char **environ;

bool safe_is_number(const char *s) {
    if (!s) return false;
    if (*s == '+') s++;
    size_t n = 0;
    for (; s[n]; n++) {
        if (!isdigit((unsigned char)s[n])) return false;
    }
    return n >= 1 && n <= 32;
}

static bool all_in(const char *s, size_t min, size_t max, const char *extra) {
    if (!s) return false;
    size_t n = 0;
    for (; s[n]; n++) {
        unsigned char c = (unsigned char)s[n];
        if (!isalnum(c) && !strchr(extra, c)) return false;
        if (n >= max) return false;
    }
    return n >= min;
}

bool safe_is_token(const char *s) { return all_in(s, 1, 128, "._-"); }

bool safe_is_host(const char *s) { return all_in(s, 1, 253, ".:-"); }

bool safe_is_route_pattern(const char *s) {
    if (!s || !*s || strlen(s) > 200) return false;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isdigit(c) || strchr("[]{}()|?+*.,-\\d", c)) continue;
        return false;
    }
    char anchored[256];
    snprintf(anchored, sizeof(anchored), "^(%s)$", s);
    regex_t re;
    if (regcomp(&re, anchored, REG_EXTENDED | REG_NOSUB) != 0) return false;
    regfree(&re);
    return true;
}

bool safe_pattern_matches(const char *pattern, const char *number) {
    if (!safe_is_route_pattern(pattern) || !safe_is_number(number)) return false;
    // POSIX regex has no \d: expand it the way FreeSWITCH (PCRE) reads it.
    char expanded[512];
    size_t o = 0;
    for (const char *p = pattern; *p && o + 6 < sizeof(expanded); p++) {
        if (p[0] == '\\' && p[1] == 'd') {
            memcpy(expanded + o, "[0-9]", 5);
            o += 5;
            p++;
        } else {
            expanded[o++] = *p;
        }
    }
    expanded[o] = '\0';
    char anchored[600];
    snprintf(anchored, sizeof(anchored), "^(%s)$", expanded);
    regex_t re;
    if (regcomp(&re, anchored, REG_EXTENDED | REG_NOSUB) != 0) return false;
    bool ok = regexec(&re, number, 0, NULL, 0) == 0;
    regfree(&re);
    return ok;
}

bool safe_ip_in_list(const char *list, const char *ip) {
    if (!list || !ip || !*ip) return false;
    size_t iplen = strlen(ip);
    const char *p = list;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == ';' || *p == '\t') p++;
        const char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != ';' && *p != '\t') p++;
        if ((size_t)(p - start) == iplen && strncmp(start, ip, iplen) == 0) return true;
    }
    return false;
}

bool safe_xml_escape(const char *in, char *out, size_t outlen) {
    if (!out || outlen == 0) return false;
    out[0] = '\0';
    if (!in) return true;
    size_t o = 0;
    for (const char *p = in; *p; p++) {
        const char *rep = NULL;
        switch (*p) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&apos;"; break;
            default: break;
        }
        size_t need = rep ? strlen(rep) : 1;
        if (o + need >= outlen) {
            out[0] = '\0';
            return false;
        }
        if (rep) {
            memcpy(out + o, rep, need);
        } else {
            out[o] = *p;
        }
        o += need;
    }
    out[o] = '\0';
    return true;
}

int safe_run(const char *const argv[]) {
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, (char *const *)argv, environ) != 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

const char *safe_env(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return (v && *v) ? v : fallback;
}
