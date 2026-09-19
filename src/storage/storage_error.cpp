#include "storage/storage_error.h"

#include "storage/database.h"

namespace chess {
namespace storage {

StorageError classify(const QueryResult& r) {
    if (r.ok) return StorageError::Ok;

    // libpq leaves sqlstate empty when the failure was at the transport
    // (bad connection, socket EOF). Everything else has a five-char code.
    if (r.sqlstate.empty()) return StorageError::Disconnected;

    const auto& s = r.sqlstate;
    if (s == "23505") return StorageError::UniqueViolation;
    if (s == "23503") return StorageError::ForeignKeyViolation;
    if (s == "23514") return StorageError::CheckViolation;
    if (s == "23502") return StorageError::NotNullViolation;
    if (s == "40001") return StorageError::SerializationFailure;
    if (s == "40P01") return StorageError::DeadlockDetected;
    if (s == "57014") return StorageError::QueryCanceled;

    // Class 22 — data exception (invalid text representation, numeric
    // overflow, etc.). Coarser than the individual codes on purpose:
    // application-layer code never wants to distinguish "invalid_datetime_format"
    // from "invalid_numeric_representation" — both mean "we sent bad input".
    if (s.size() >= 2 && s[0] == '2' && s[1] == '2') return StorageError::InvalidInput;

    return StorageError::Unknown;
}

const char* to_string(StorageError e) {
    switch (e) {
        case StorageError::Ok:                    return "ok";
        case StorageError::Disconnected:          return "disconnected";
        case StorageError::UniqueViolation:       return "unique_violation";
        case StorageError::ForeignKeyViolation:   return "foreign_key_violation";
        case StorageError::CheckViolation:        return "check_violation";
        case StorageError::NotNullViolation:      return "not_null_violation";
        case StorageError::SerializationFailure:  return "serialization_failure";
        case StorageError::DeadlockDetected:      return "deadlock_detected";
        case StorageError::QueryCanceled:         return "query_canceled";
        case StorageError::InvalidInput:          return "invalid_input";
        case StorageError::Unknown:               return "unknown";
    }
    return "unknown";
}

bool is_retriable(StorageError e) {
    return e == StorageError::SerializationFailure
        || e == StorageError::DeadlockDetected;
}

} // namespace storage
} // namespace chess
