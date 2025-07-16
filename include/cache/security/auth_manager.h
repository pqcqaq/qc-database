#pragma once

#include "cache/common/types.h"
#include <string>
#include <vector>
#include <unordered_set>
#include <memory>
#include <chrono>

namespace cache {
namespace security {

// Permission types
enum class Permission {
    READ,
    WRITE,
    DELETE,
    ADMIN,
    STATS
};

// User information
struct User {
    std::string user_id;
    std::string username;
    std::string password_hash;
    std::unordered_set<Permission> permissions;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_login;
    bool is_active;
    
    User() : is_active(true) {
        created_at = std::chrono::system_clock::now();
    }
    
    bool has_permission(Permission perm) const {
        return permissions.find(perm) != permissions.end();
    }
};

// Authentication token
struct AuthToken {
    std::string token;
    std::string user_id;
    std::chrono::system_clock::time_point issued_at;
    std::chrono::system_clock::time_point expires_at;
    bool is_valid;
    
    AuthToken() : is_valid(false) {}
    
    bool is_expired() const {
        return std::chrono::system_clock::now() > expires_at;
    }
};

// Access control entry
struct ACLEntry {
    std::string resource_pattern;  // Key pattern or namespace
    std::string user_id;
    std::unordered_set<Permission> permissions;
    std::chrono::system_clock::time_point created_at;
    
    ACLEntry() {
        created_at = std::chrono::system_clock::now();
    }
    
    bool matches_resource(const std::string& resource) const;
};

// Authentication result
struct AuthResult {
    bool success;
    std::string user_id;
    std::unordered_set<Permission> permissions;
    std::string error_message;
    
    AuthResult() : success(false) {}
    AuthResult(bool s, const std::string& uid, const std::unordered_set<Permission>& perms)
        : success(s), user_id(uid), permissions(perms) {}
};

// Authentication manager interface
class AuthManager {
public:
    virtual ~AuthManager() = default;
    
    // User management
    virtual Result<void> create_user(const std::string& username, const std::string& password,
                                    const std::unordered_set<Permission>& permissions) = 0;
    virtual Result<void> delete_user(const std::string& username) = 0;
    virtual Result<void> update_user_permissions(const std::string& username,
                                                 const std::unordered_set<Permission>& permissions) = 0;
    virtual Result<void> change_password(const std::string& username, const std::string& old_password,
                                        const std::string& new_password) = 0;
    virtual Result<User> get_user(const std::string& username) = 0;
    virtual std::vector<User> list_users() = 0;
    
    // Authentication
    virtual Result<AuthToken> authenticate(const std::string& username, const std::string& password) = 0;
    virtual Result<AuthResult> validate_token(const std::string& token) = 0;
    virtual Result<void> revoke_token(const std::string& token) = 0;
    virtual Result<void> revoke_all_tokens(const std::string& user_id) = 0;
    
    // Authorization
    virtual bool check_permission(const std::string& user_id, Permission permission,
                                 const std::string& resource = "") = 0;
    virtual Result<void> grant_permission(const std::string& user_id, Permission permission,
                                         const std::string& resource_pattern = "*") = 0;
    virtual Result<void> revoke_permission(const std::string& user_id, Permission permission,
                                          const std::string& resource_pattern = "*") = 0;
    
    // Access control lists
    virtual Result<void> add_acl_entry(const ACLEntry& entry) = 0;
    virtual Result<void> remove_acl_entry(const std::string& user_id, const std::string& resource_pattern) = 0;
    virtual std::vector<ACLEntry> list_acl_entries(const std::string& user_id = "") = 0;
    
    // Configuration
    virtual void set_token_expiry_time(std::chrono::seconds expiry_time) = 0;
    virtual void set_password_policy(size_t min_length, bool require_special_chars) = 0;
    virtual void enable_audit_logging(bool enable) = 0;
    
    // Statistics
    virtual size_t get_active_sessions() = 0;
    virtual std::vector<std::string> get_login_history(const std::string& user_id, size_t limit = 100) = 0;
};

// Simple in-memory authentication manager
class InMemoryAuthManager : public AuthManager {
public:
    InMemoryAuthManager();
    ~InMemoryAuthManager() override;
    
    // AuthManager interface implementation
    Result<void> create_user(const std::string& username, const std::string& password,
                            const std::unordered_set<Permission>& permissions) override;
    Result<void> delete_user(const std::string& username) override;
    Result<void> update_user_permissions(const std::string& username,
                                        const std::unordered_set<Permission>& permissions) override;
    Result<void> change_password(const std::string& username, const std::string& old_password,
                                const std::string& new_password) override;
    Result<User> get_user(const std::string& username) override;
    std::vector<User> list_users() override;
    
    Result<AuthToken> authenticate(const std::string& username, const std::string& password) override;
    Result<AuthResult> validate_token(const std::string& token) override;
    Result<void> revoke_token(const std::string& token) override;
    Result<void> revoke_all_tokens(const std::string& user_id) override;
    
    bool check_permission(const std::string& user_id, Permission permission,
                         const std::string& resource = "") override;
    Result<void> grant_permission(const std::string& user_id, Permission permission,
                                 const std::string& resource_pattern = "*") override;
    Result<void> revoke_permission(const std::string& user_id, Permission permission,
                                  const std::string& resource_pattern = "*") override;
    
    Result<void> add_acl_entry(const ACLEntry& entry) override;
    Result<void> remove_acl_entry(const std::string& user_id, const std::string& resource_pattern) override;
    std::vector<ACLEntry> list_acl_entries(const std::string& user_id = "") override;
    
    void set_token_expiry_time(std::chrono::seconds expiry_time) override;
    void set_password_policy(size_t min_length, bool require_special_chars) override;
    void enable_audit_logging(bool enable) override;
    
    size_t get_active_sessions() override;
    std::vector<std::string> get_login_history(const std::string& user_id, size_t limit = 100) override;
    
private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, User> users_;  // username -> user
    std::unordered_map<std::string, AuthToken> tokens_;  // token -> auth_token
    std::vector<ACLEntry> acl_entries_;
    
    // Configuration
    std::chrono::seconds token_expiry_;
    size_t min_password_length_;
    bool require_special_chars_;
    bool audit_logging_enabled_;
    
    // Audit log
    std::vector<std::string> audit_log_;
    
    // Helper methods
    std::string generate_token();
    std::string hash_password(const std::string& password);
    bool verify_password(const std::string& password, const std::string& hash);
    bool validate_password_policy(const std::string& password);
    void log_auth_event(const std::string& event, const std::string& user_id = "",
                       const std::string& details = "");
    void cleanup_expired_tokens();
    
    // Default admin user creation
    void create_default_admin();
};

// Utility functions
std::string permission_to_string(Permission perm);
Permission string_to_permission(const std::string& str);
std::unordered_set<Permission> parse_permissions_string(const std::string& perms_str);

} // namespace security
} // namespace cache