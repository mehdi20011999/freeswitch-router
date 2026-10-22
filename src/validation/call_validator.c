// src/validation/call_validator.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "validation/call_validator.h"
#include "core/logging.h"
#include "freeswitch/fs_xml_generator.h"
#include "core/safety.h"

static void hangup_and_release(call_validator_t *validator, const char *call_id, const char *assigned_did);

// Find a validation record. The caller must hold validation_mutex for as long
// as it uses the returned pointer: the validation thread frees records.
static call_validation_record_t* find_validation_record_locked(call_validator_t *validator,
                                                               const char *call_id) {
    for (call_validation_record_t *r = validator->active_validations; r; r = r->next) {
        if (strcmp(r->call_id, call_id) == 0) return r;
    }
    return NULL;
}

// Helper function to create validation record
static call_validation_record_t* create_validation_record(const char *call_id,
                                                          const char *ani,
                                                          const char *dnis) {
    call_validation_record_t *record = calloc(1, sizeof(call_validation_record_t));
    if (!record) return NULL;
    
    strncpy(record->call_id, call_id, sizeof(record->call_id) - 1);
    strncpy(record->origin_ani, ani, sizeof(record->origin_ani) - 1);
    strncpy(record->origin_dnis, dnis, sizeof(record->origin_dnis) - 1);
    
    record->current_state = CALL_STATE_ORIGINATED;
    record->expected_state = CALL_STATE_RINGING;
    record->originated_at = time(NULL);
    record->state_timestamp = record->originated_at;
    record->validation_attempts = 0;
    
    // Generate UUID for tracking
    snprintf(record->call_uuid, sizeof(record->call_uuid), 
            "%ld-%s", time(NULL), call_id);
    
    return record;
}

// Validation thread function
static void* validation_thread_func(void *arg) {
    call_validator_t *validator = (call_validator_t*)arg;
    
    LOG_INFO("Call validation thread started");
    
    while (validator->running) {
        time_t now = time(NULL);
        
        pthread_mutex_lock(&validator->validation_mutex);
        
        call_validation_record_t **pp = &validator->active_validations;
        while (*pp) {
            call_validation_record_t *record = *pp;
            bool should_remove = false;
            
            // Check validation timeout (30 seconds without state change)
            if ((now - record->state_timestamp) > 30) {
                if (record->current_state == CALL_STATE_ORIGINATED) {
                    LOG_WARN("Call %s: No ringing confirmation after 30s - forcing hangup",
                            record->call_id);
                    
                    // Log security event
                    log_security_event(validator, "VALIDATION_TIMEOUT", "HIGH",
                                     record->call_id, 
                                     "Call did not progress to ringing state");
                    
                    // Force hangup to avoid charges. The internal variant does not
                    // take validation_mutex again (we hold it: that was a deadlock).
                    hangup_and_release(validator, record->call_id, record->assigned_did);
                    
                    record->current_state = CALL_STATE_FAILED;
                    validator->stats.failed_validations++;
                    should_remove = true;
                }
            }
            
            // Check for state mismatches
            if (record->current_state != record->expected_state && 
                record->validation_attempts > 3) {
                LOG_ERROR("Call %s: State mismatch detected - Expected: %d, Current: %d",
                         record->call_id, record->expected_state, record->current_state);
                
                log_security_event(validator, "STATE_MISMATCH", "CRITICAL",
                                 record->call_id,
                                 "Call state does not match expected progression");
                
                hangup_and_release(validator, record->call_id, record->assigned_did);
                validator->stats.diverted_calls++;
                should_remove = true;
            }
            
            // Perform periodic validation checks
            if ((now - record->last_validation) >= validator->validation_interval_ms / 1000) {
                record->validation_attempts++;
                record->last_validation = now;
                
                // Query the call state reported for this call. (The original
                // query used columns that active_calls does not have.)
                const char *params[] = {record->call_id};
                db_result_t *result = db_query_params(validator->db,
                        "SELECT status FROM active_calls WHERE call_uuid = $1", 1, params);
                if (result && result->num_rows > 0) {
                    const char *remote_status = db_get_value(result, 0, 0);
                    
                    // Validate state consistency
                    if (strcmp(remote_status, "ringing") == 0 && 
                        record->current_state == CALL_STATE_ORIGINATED) {
                        record->current_state = CALL_STATE_RINGING;
                        record->expected_state = CALL_STATE_ANSWERED;
                        record->destination_validated = true;
                        LOG_INFO("Call %s: Validated ringing at destination", record->call_id);
                    } else if (strcmp(remote_status, "answered") == 0 &&
                             record->current_state == CALL_STATE_RINGING) {
                        record->current_state = CALL_STATE_ANSWERED;
                        record->expected_state = CALL_STATE_HUNG_UP;
                        record->answered_at = now;
                        LOG_INFO("Call %s: Validated answered at destination", record->call_id);
                    }
                    
                    db_free_result(result);
                } else {
                    LOG_WARN("Call %s: Cannot verify state with destination server",
                            record->call_id);
                }
            }
            
            // Remove completed or failed calls after 60 seconds
            if ((record->current_state == CALL_STATE_HUNG_UP ||
                 record->current_state == CALL_STATE_FAILED) &&
                (now - record->state_timestamp) > 60) {
                should_remove = true;
            }
            
            if (should_remove) {
                *pp = record->next;
                free(record);
            } else {
                pp = &record->next;
            }
        }
        
        pthread_mutex_unlock(&validator->validation_mutex);
        
        // Sleep for the validation interval in short steps, so stop() is quick.
        for (int waited = 0; waited < validator->validation_interval_ms && validator->running; waited += 100) {
            usleep(100 * 1000);
        }
    }
    
    LOG_INFO("Call validation thread stopped");
    return NULL;
}

// Public API implementation
call_validator_t* call_validator_create(database_t *db, router_t *router) {
    call_validator_t *validator = calloc(1, sizeof(call_validator_t));
    if (!validator) return NULL;
    
    validator->db = db;
    validator->router = router;
    validator->running = false;
    
    // Default configuration
    validator->validation_interval_ms = 5000;  // Check every 5 seconds
    validator->validation_timeout_ms = 30000;  // 30 second timeout
    validator->max_validation_attempts = 5;
    validator->strict_mode = true;
    
    pthread_mutex_init(&validator->validation_mutex, NULL);
    
    // Tables and default rules live in sql/001_schema.sql (they used to be
    // created here on every start, adding 5 duplicate rules each time).

    LOG_INFO("Call validator created with interval=%dms, timeout=%dms",
            validator->validation_interval_ms, validator->validation_timeout_ms);
    
    return validator;
}

void call_validator_destroy(call_validator_t *validator) {
    if (!validator) return;
    
    call_validator_stop(validator);
    
    // Clean up active validations
    pthread_mutex_lock(&validator->validation_mutex);
    
    call_validation_record_t *record = validator->active_validations;
    while (record) {
        call_validation_record_t *next = record->next;
        free(record);
        record = next;
    }
    
    pthread_mutex_unlock(&validator->validation_mutex);
    pthread_mutex_destroy(&validator->validation_mutex);
    
    free(validator);
}

int call_validator_start(call_validator_t *validator) {
    if (!validator || validator->running) return -1;
    
    validator->running = true;
    
    if (pthread_create(&validator->validation_thread, NULL, 
                      validation_thread_func, validator) != 0) {
        LOG_ERROR("Failed to create validation thread");
        validator->running = false;
        return -1;
    }
    
    LOG_INFO("Call validator started");
    return 0;
}

void call_validator_stop(call_validator_t *validator) {
    if (!validator || !validator->running) return;
    
    validator->running = false;
    pthread_join(validator->validation_thread, NULL);
    
    LOG_INFO("Call validator stopped");
}

// Validate call initiation
validation_result_t validate_call_initiation(call_validator_t *validator,
                                            const char *call_id,
                                            const char *ani,
                                            const char *dnis,
                                            const char *source_ip) {
    if (!validator || !call_id || !ani || !dnis || !source_ip) {
        return VALIDATION_FAILED;
    }
    
    LOG_INFO("Validating call initiation: ID=%s, ANI=%s, DNIS=%s, Source=%s",
            call_id, ani, dnis, source_ip);
    
    // Check if source IP is authorized
    if (!verify_server_authorization(validator, source_ip, call_id)) {
        LOG_ERROR("Call %s: Unauthorized source IP %s", call_id, source_ip);
        log_security_event(validator, "UNAUTHORIZED_SOURCE", "CRITICAL",
                         call_id, "Unauthorized source IP attempted call");
        return VALIDATION_UNAUTHORIZED;
    }
    
    // Create validation record
    call_validation_record_t *record = create_validation_record(call_id, ani, dnis);
    if (!record) {
        LOG_ERROR("Failed to create validation record for call %s", call_id);
        return VALIDATION_FAILED;
    }
    
    strncpy(record->origin_server_ip, source_ip, sizeof(record->origin_server_ip) - 1);
    record->origin_validated = true;
    
    // Copy what we still need: once linked, the record belongs to the list.
    char call_uuid[sizeof(record->call_uuid)];
    snprintf(call_uuid, sizeof(call_uuid), "%s", record->call_uuid);

    // Add to active validations
    pthread_mutex_lock(&validator->validation_mutex);
    record->next = validator->active_validations;
    validator->active_validations = record;
    pthread_mutex_unlock(&validator->validation_mutex);
    
    // Record in database
    char state[16];
    snprintf(state, sizeof(state), "%d", CALL_STATE_ORIGINATED);
    const char *params[] = {call_id, call_uuid, ani, dnis, state};
    db_free_result(db_query_params(validator->db,
            "INSERT INTO call_validations (call_id, call_uuid, origin_ani, origin_dnis, "
            "current_state, validation_status) VALUES ($1, $2, $3, $4, $5::int, 'active') "
            "ON CONFLICT (call_id) DO NOTHING", 5, params));
    
    // Record checkpoint
    record_validation_checkpoint(validator, "call_initiated", 0, call_id, true, NULL);
    
    validator->stats.total_validations++;
    
    return VALIDATION_SUCCESS;
}

// Validate call progress
validation_result_t validate_call_progress(call_validator_t *validator,
                                          const char *call_id,
                                          call_state_t new_state,
                                          const char *server_ip) {
    if (!validator || !call_id || !server_ip) {
        return VALIDATION_FAILED;
    }
    
    pthread_mutex_lock(&validator->validation_mutex);
    call_validation_record_t *record = find_validation_record_locked(validator, call_id);
    if (!record) {
        pthread_mutex_unlock(&validator->validation_mutex);
        LOG_ERROR("Call %s: No validation record found", call_id);
        return VALIDATION_FAILED;
    }
    
    // Verify state progression is valid
    bool valid_transition = false;
    call_state_t previous_state = record->current_state;
    
    switch (record->current_state) {
        case CALL_STATE_ORIGINATED:
            valid_transition = (new_state == CALL_STATE_RINGING);
            break;
        case CALL_STATE_RINGING:
            valid_transition = (new_state == CALL_STATE_ANSWERED || 
                              new_state == CALL_STATE_HUNG_UP);
            break;
        case CALL_STATE_ANSWERED:
            valid_transition = (new_state == CALL_STATE_HUNG_UP);
            break;
        default:
            valid_transition = false;
    }
    
    if (!valid_transition) {
        pthread_mutex_unlock(&validator->validation_mutex);
        LOG_ERROR("Call %s: Invalid state transition from %d to %d",
                 call_id, previous_state, new_state);
        
        log_security_event(validator, "INVALID_STATE_TRANSITION", "HIGH",
                         call_id, "Invalid call state progression detected");
        
        record_validation_checkpoint(validator, "state_transition", 0, call_id, 
                                   false, "Invalid state transition");
        
        return VALIDATION_MISMATCH;
    }
    
    // Update state (still under the lock taken above)
    record->current_state = new_state;
    record->state_timestamp = time(NULL);
    
    if (new_state == CALL_STATE_ANSWERED) {
        record->answered_at = record->state_timestamp;
    } else if (new_state == CALL_STATE_HUNG_UP) {
        record->ended_at = record->state_timestamp;
        if (record->answered_at > 0) {
            record->duration_ms = (record->ended_at - record->answered_at) * 1000;
        }
    }
    
    pthread_mutex_unlock(&validator->validation_mutex);
    
    // Update database
    char state[16];
    snprintf(state, sizeof(state), "%d", (int)new_state);
    const char *params[] = {state, call_id};
    db_free_result(db_query_params(validator->db,
            "UPDATE call_validations SET current_state = $1::int, updated_at = CURRENT_TIMESTAMP "
            "WHERE call_id = $2", 2, params));
    
    // Record checkpoint
    char checkpoint_name[128];
    snprintf(checkpoint_name, sizeof(checkpoint_name), "state_%d", new_state);
    record_validation_checkpoint(validator, checkpoint_name, 0, call_id, true, NULL);
    
    LOG_INFO("Call %s: State updated to %d", call_id, new_state);
    
    return VALIDATION_SUCCESS;
}

// Validate call routing
validation_result_t validate_call_routing(call_validator_t *validator,
                                         const char *call_id,
                                         const char *expected_destination,
                                         const char *actual_destination) {
    if (!validator || !call_id || !expected_destination || !actual_destination) {
        return VALIDATION_FAILED;
    }
    
    pthread_mutex_lock(&validator->validation_mutex);
    call_validation_record_t *record = find_validation_record_locked(validator, call_id);
    if (!record) {
        pthread_mutex_unlock(&validator->validation_mutex);
        LOG_ERROR("Call %s: No validation record found", call_id);
        return VALIDATION_FAILED;
    }
    
    // Check if actual destination matches expected
    if (strcmp(expected_destination, actual_destination) != 0) {
        record->current_state = CALL_STATE_DIVERTED;
        pthread_mutex_unlock(&validator->validation_mutex);
        LOG_ERROR("Call %s: Route mismatch - Expected: %s, Actual: %s",
                 call_id, expected_destination, actual_destination);
        
        log_security_event(validator, "ROUTE_DIVERSION", "CRITICAL",
                         call_id, "Call diverted to unexpected destination");
        
        record_validation_checkpoint(validator, "route_validation", 0, call_id,
                                   false, "Destination mismatch");
        
        force_hangup_invalid_call(validator, call_id);
        
        validator->stats.diverted_calls++;
        
        return VALIDATION_DIVERTED;
    }
    
    // Update validation record (lock still held)
    strncpy(record->destination_server_ip, actual_destination, 
           sizeof(record->destination_server_ip) - 1);
    record->route_validated = true;
    pthread_mutex_unlock(&validator->validation_mutex);
    
    record_validation_checkpoint(validator, "route_validation", 0, call_id,
                               true, NULL);
    
    LOG_INFO("Call %s: Route validated to %s", call_id, actual_destination);
    
    return VALIDATION_SUCCESS;
}

// Validate DID assignment
validation_result_t validate_did_assignment(call_validator_t *validator,
                                           const char *call_id,
                                           const char *did,
                                           const char *server_ip) {
    if (!validator || !call_id || !did || !server_ip) {
        return VALIDATION_FAILED;
    }
    
    // Check DID ownership and availability
    const char *params[] = {did};
    db_result_t *result = db_query_params(validator->db,
            "SELECT id, in_use, call_id FROM dids WHERE did = $1 AND active", 1, params);
    if (!result || result->num_rows == 0) {
        LOG_ERROR("Call %s: DID %s not found or inactive", call_id, did);
        
        log_security_event(validator, "INVALID_DID", "HIGH",
                         call_id, "Attempted to use invalid or inactive DID");
        
        db_free_result(result);
        return VALIDATION_FAILED;
    }
    
    // Check if DID is already in use
    int in_use = atoi(db_get_value(result, 0, 1));
    const char *existing_call_id = db_get_value(result, 0, 2);
    
    if (in_use && existing_call_id && strcmp(existing_call_id, call_id) != 0) {
        LOG_ERROR("Call %s: DID %s already in use by call %s",
                 call_id, did, existing_call_id);
        
        log_security_event(validator, "DID_COLLISION", "HIGH",
                         call_id, "DID already assigned to another call");
        
        db_free_result(result);
        return VALIDATION_FAILED;
    }
    
    db_free_result(result);
    
    // Update validation record
    pthread_mutex_lock(&validator->validation_mutex);
    call_validation_record_t *record = find_validation_record_locked(validator, call_id);
    if (record) {
        strncpy(record->assigned_did, did, sizeof(record->assigned_did) - 1);
        record->did_ownership_verified = true;
    }
    pthread_mutex_unlock(&validator->validation_mutex);
    
    record_validation_checkpoint(validator, "did_assignment", 0, call_id,
                               true, NULL);
    
    LOG_INFO("Call %s: DID %s assignment validated", call_id, did);
    
    return VALIDATION_SUCCESS;
}

// Update call state
int update_call_state(call_validator_t *validator,
                     const char *call_id,
                     call_state_t new_state) {
    return validate_call_progress(validator, call_id, new_state, "127.0.0.1");
}

// Get call state
call_state_t get_call_state(call_validator_t *validator,
                           const char *call_id) {
    pthread_mutex_lock(&validator->validation_mutex);
    call_validation_record_t *record = find_validation_record_locked(validator, call_id);
    call_state_t state = record ? record->current_state : CALL_STATE_HUNG_UP;
    pthread_mutex_unlock(&validator->validation_mutex);
    return state;
}

// Verify server authorization
bool verify_server_authorization(call_validator_t *validator,
                                const char *server_ip,
                                const char *call_id) {
    const char *params[] = {server_ip};
    db_result_t *result = db_query_params(validator->db,
            "SELECT id FROM providers WHERE host = $1 AND active", 1, params);
    bool authorized = (result && result->num_rows > 0);
    
    if (!authorized) {
        LOG_WARN("Server %s not authorized for call %s", server_ip, call_id);
    }
    
    if (result) db_free_result(result);
    return authorized;
}

// Verify route integrity
bool verify_route_integrity(call_validator_t *validator,
                           const char *call_id,
                           int origin_id,
                           int intermediate_id,
                           int final_id) {
    // Verify that the route exists and is active
    char query[512];
    snprintf(query, sizeof(query),
            "SELECT id FROM routes WHERE "
            "origin_provider_id = %d AND "
            "intermediate_provider_id = %d AND "
            "final_provider_id = %d AND "
            "active = true",
            origin_id, intermediate_id, final_id);
    
    db_result_t *result = db_query(validator->db, query);
    bool valid = (result && result->num_rows > 0);
    
    if (!valid) {
        LOG_ERROR("Call %s: Route integrity check failed for %d->%d->%d",
                 call_id, origin_id, intermediate_id, final_id);
    }
    
    if (result) db_free_result(result);
    return valid;
}

// Log security event
void log_security_event(call_validator_t *validator,
                       const char *event_type,
                       const char *severity,
                       const char *call_id,
                       const char *details) {
    const char *params[] = {event_type, severity, call_id, details};
    db_free_result(db_query_params(validator->db,
            "INSERT INTO security_events (event_type, severity, call_id, details) "
            "VALUES ($1, $2, $3, $4)", 4, params));
    
    validator->stats.security_events++;
    
    LOG_WARN("SECURITY EVENT: Type=%s, Severity=%s, Call=%s, Details=%s",
            event_type, severity, call_id, details);
}

// Record validation checkpoint
void record_validation_checkpoint(call_validator_t *validator,
                                 const char *checkpoint_name,
                                 int route_id,
                                 const char *call_id,
                                 bool passed,
                                 const char *failure_reason) {
    char route[16];
    snprintf(route, sizeof(route), "%d", route_id);
    const char *params[] = {checkpoint_name, route, call_id, passed ? "true" : "false", failure_reason};
    db_free_result(db_query_params(validator->db,
            "INSERT INTO validation_checkpoints (checkpoint_name, route_id, call_id, passed, failure_reason) "
            "VALUES ($1, $2::int, $3, $4::boolean, $5)", 5, params));
    
    LOG_DEBUG("Checkpoint %s for call %s: %s",
             checkpoint_name, call_id, passed ? "PASSED" : "FAILED");
}

// Get validation statistics
void get_validation_stats(call_validator_t *validator,
                         uint64_t *total,
                         uint64_t *successful,
                         uint64_t *failed,
                         uint64_t *diverted) {
    if (total) *total = validator->stats.total_validations;
    if (successful) *successful = validator->stats.successful_validations;
    if (failed) *failed = validator->stats.failed_validations;
    if (diverted) *diverted = validator->stats.diverted_calls;
}

// Clean up expired validations
void cleanup_expired_validations(call_validator_t *validator) {
    time_t now = time(NULL);
    time_t expiry_time = now - 3600; // 1 hour expiry
    
    pthread_mutex_lock(&validator->validation_mutex);
    
    call_validation_record_t **pp = &validator->active_validations;
    while (*pp) {
        call_validation_record_t *record = *pp;
        
        if (record->ended_at > 0 && record->ended_at < expiry_time) {
            *pp = record->next;
            
            const char *params[] = {record->call_id};
            db_free_result(db_query_params(validator->db,
                    "UPDATE call_validations SET validation_status = 'expired' WHERE call_id = $1",
                    1, params));
            
            free(record);
        } else {
            pp = &record->next;
        }
    }
    
    pthread_mutex_unlock(&validator->validation_mutex);
    
    // Clean up old database records
    char query[256];
    snprintf(query, sizeof(query),
            "DELETE FROM call_validations WHERE created_at < NOW() - INTERVAL '24 hours'");
    db_query(validator->db, query);
    
    snprintf(query, sizeof(query),
            "DELETE FROM validation_checkpoints WHERE checked_at < NOW() - INTERVAL '7 days'");
    db_query(validator->db, query);
    
    snprintf(query, sizeof(query),
            "DELETE FROM security_events WHERE created_at < NOW() - INTERVAL '30 days'");
    db_query(validator->db, query);
}

// Hang up a call that failed validation and free its DID. Takes no lock, so
// the validation thread can call it while it holds validation_mutex.
static void hangup_and_release(call_validator_t *validator, const char *call_id, const char *assigned_did) {
    LOG_WARN("Forcing hangup of invalid call: %s", call_id);

    // No shell: the call id comes from the network. Only well-formed ids are
    // passed to fs_cli, as a separate argument.
    if (safe_is_token(call_id)) {
        char api[192];
        snprintf(api, sizeof(api), "uuid_kill %s VALIDATION_FAILED", call_id);
        const char *argv[] = {safe_env("ROUTER_FS_CLI", "fs_cli"), "-x", api, NULL};
        if (safe_run(argv) != 0) LOG_WARN("fs_cli uuid_kill failed for %s", call_id);
    } else {
        LOG_ERROR("Refusing to pass malformed call id to fs_cli");
    }

    char state[16];
    snprintf(state, sizeof(state), "%d", CALL_STATE_FAILED);
    const char *p1[] = {call_id};
    db_free_result(db_query_params(validator->db,
            "UPDATE call_records SET status = 'failed', ended_at = CURRENT_TIMESTAMP, "
            "failure_reason = 'Validation failed' WHERE call_id = $1", 1, p1));
    const char *p2[] = {state, call_id};
    db_free_result(db_query_params(validator->db,
            "UPDATE call_validations SET validation_status = 'failed', current_state = $1::int "
            "WHERE call_id = $2", 2, p2));

    if (assigned_did && assigned_did[0]) {
        const char *p3[] = {assigned_did, call_id};
        db_free_result(db_query_params(validator->db, "SELECT router_cancel_did($1, $2, 'Validation failed')",
                                       2, p3));
    }
}

void force_hangup_invalid_call(call_validator_t *validator, const char *call_id) {
    char did[sizeof(((call_validation_record_t *)0)->assigned_did)] = "";
    pthread_mutex_lock(&validator->validation_mutex);
    for (call_validation_record_t *r = validator->active_validations; r; r = r->next) {
        if (strcmp(r->call_id, call_id) == 0) {
            snprintf(did, sizeof(did), "%s", r->assigned_did);
            break;
        }
    }
    pthread_mutex_unlock(&validator->validation_mutex);
    hangup_and_release(validator, call_id, did);
}
