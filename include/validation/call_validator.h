// include/validation/call_validator.h
#ifndef CALL_VALIDATOR_H
#define CALL_VALIDATOR_H

#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include "db/database.h"
#include "router/router.h"

// Call states as per PDF requirements
typedef enum {
    CALL_STATE_ORIGINATED = 1,    // Call initiated from origin
    CALL_STATE_RINGING = 2,       // Call ringing at destination
    CALL_STATE_ANSWERED = 3,       // Call answered at destination
    CALL_STATE_HUNG_UP = 4,        // Call ended
    CALL_STATE_FAILED = 5,         // Call failed validation
    CALL_STATE_DIVERTED = 6        // Call was diverted (security issue)
} call_state_t;

// Validation result codes
typedef enum {
    VALIDATION_SUCCESS = 0,
    VALIDATION_FAILED = -1,
    VALIDATION_TIMEOUT = -2,
    VALIDATION_DIVERTED = -3,
    VALIDATION_MISMATCH = -4,
    VALIDATION_UNAUTHORIZED = -5
} validation_result_t;

// Call validation record
typedef struct call_validation_record {
    char call_id[256];
    char call_uuid[64];
    char origin_ani[64];
    char origin_dnis[64];
    char assigned_did[64];
    
    // Server tracking
    int origin_server_id;
    int intermediate_server_id;
    int final_server_id;
    char origin_server_ip[64];
    char destination_server_ip[64];
    
    // State tracking
    call_state_t current_state;
    call_state_t expected_state;
    time_t state_timestamp;
    
    // Validation data
    bool origin_validated;
    bool destination_validated;
    bool route_validated;
    int validation_attempts;
    time_t last_validation;
    
    // Security checks
    bool ip_match_verified;
    bool did_ownership_verified;
    bool route_integrity_verified;
    
    // Statistics
    time_t originated_at;
    time_t answered_at;
    time_t ended_at;
    int duration_ms;
    
    struct call_validation_record *next;
} call_validation_record_t;

// Validation checkpoint structure
typedef struct validation_checkpoint {
    char checkpoint_name[128];
    int route_id;
    char call_id[256];
    bool passed;
    char failure_reason[256];
    time_t checked_at;
} validation_checkpoint_t;

// Security event structure
typedef struct security_event {
    char event_type[64];
    char severity[32];
    char call_id[256];
    char source_ip[64];
    char details[512];
    time_t created_at;
} security_event_t;

// Call validator structure
typedef struct call_validator {
    database_t *db;
    router_t *router;
    
    // Active validation records
    call_validation_record_t *active_validations;
    pthread_mutex_t validation_mutex;
    
    // Validation thread
    pthread_t validation_thread;
    atomic_bool running;  // read by the validation thread
    
    // Configuration
    int validation_interval_ms;
    int validation_timeout_ms;
    int max_validation_attempts;
    bool strict_mode;
    
    // Statistics
    struct {
        uint64_t total_validations;
        uint64_t successful_validations;
        uint64_t failed_validations;
        uint64_t diverted_calls;
        uint64_t security_events;
    } stats;
} call_validator_t;

// Public API functions
call_validator_t* call_validator_create(database_t *db, router_t *router);
void call_validator_destroy(call_validator_t *validator);
int call_validator_start(call_validator_t *validator);
void call_validator_stop(call_validator_t *validator);

// Validation operations
validation_result_t validate_call_initiation(call_validator_t *validator,
                                            const char *call_id,
                                            const char *ani,
                                            const char *dnis,
                                            const char *source_ip);

validation_result_t validate_call_progress(call_validator_t *validator,
                                          const char *call_id,
                                          call_state_t new_state,
                                          const char *server_ip);

validation_result_t validate_call_routing(call_validator_t *validator,
                                         const char *call_id,
                                         const char *expected_destination,
                                         const char *actual_destination);

validation_result_t validate_did_assignment(call_validator_t *validator,
                                           const char *call_id,
                                           const char *did,
                                           const char *server_ip);

// State management
int update_call_state(call_validator_t *validator,
                     const char *call_id,
                     call_state_t new_state);

call_state_t get_call_state(call_validator_t *validator,
                           const char *call_id);

// Security operations
bool verify_server_authorization(call_validator_t *validator,
                                const char *server_ip,
                                const char *call_id);

bool verify_route_integrity(call_validator_t *validator,
                           const char *call_id,
                           int origin_id,
                           int intermediate_id,
                           int final_id);

void log_security_event(call_validator_t *validator,
                       const char *event_type,
                       const char *severity,
                       const char *call_id,
                       const char *details);

// Checkpoint operations
void record_validation_checkpoint(call_validator_t *validator,
                                 const char *checkpoint_name,
                                 int route_id,
                                 const char *call_id,
                                 bool passed,
                                 const char *failure_reason);

// Statistics and reporting
void get_validation_stats(call_validator_t *validator,
                         uint64_t *total,
                         uint64_t *successful,
                         uint64_t *failed,
                         uint64_t *diverted);

// Cleanup operations
void cleanup_expired_validations(call_validator_t *validator);
void force_hangup_invalid_call(call_validator_t *validator, const char *call_id);

#endif // CALL_VALIDATOR_H
