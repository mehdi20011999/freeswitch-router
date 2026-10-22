#ifndef API_HANDLERS_H
#define API_HANDLERS_H

#include <microhttpd.h>

// Read-only HTTP API (daemon mode):
//   GET /health                    200 if the database answers, else 503
//   GET /api/v1/stats              providers, routes, DID pool and SIP counters
//   GET /api/v1/route?number=...   which route and intermediate gateway serve a number
enum MHD_Result handle_api_request(struct MHD_Connection *connection, const char *url, const char *method);

#endif
