// safety.h - input validation, escaping and process helpers shared by the router.
//
// Everything that comes from SIP, the database or the command line passes
// through one of these before it reaches SQL, XML, a file path or a process.
#ifndef CORE_SAFETY_H
#define CORE_SAFETY_H

#include <stdbool.h>
#include <stddef.h>

// "+"? followed by 1..32 digits.
bool safe_is_number(const char *s);

// 1..128 characters from [A-Za-z0-9._-]: gateway names, uuids, call ids.
bool safe_is_token(const char *s);

// Hostname or IPv4/IPv6 literal: 1..253 characters from [A-Za-z0-9.:-].
bool safe_is_host(const char *s);

// A route pattern is a POSIX extended regex over digits. Only the characters a
// number pattern needs are accepted (digits, [ ] { } ( ) | ? + * . , - \ d),
// so a pattern can never break out of the XML attribute it is written into.
// The pattern must also compile.
bool safe_is_route_pattern(const char *s);

// Does `number` match `pattern` completely (as ^(pattern)$)?
bool safe_pattern_matches(const char *pattern, const char *number);

// Exact membership of `ip` in a list separated by commas, spaces or semicolons.
// (The original code used strstr, so "10.0.0.1" also matched "10.0.0.10".)
bool safe_ip_in_list(const char *list, const char *ip);

// XML-escape `in` into `out`. Returns false (and writes "") if it does not fit.
bool safe_xml_escape(const char *in, char *out, size_t outlen);

// Run a program with arguments, without a shell. Returns its exit status, or
// -1 if it could not be started. argv[0] is looked up in PATH.
int safe_run(const char *const argv[]);

// getenv() with a default for unset or empty variables.
const char *safe_env(const char *name, const char *fallback);

#endif
