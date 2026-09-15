/**
 * secure_buffer.h — A byte buffer that wipes itself on destruction.
 *
 * Every secret in this project (passwords, shared secrets, derived keys,
 * HMAC keys) lives in a SecureBuffer rather than a bare std::vector<uint8_t>.
 *
 * Why this matters:
 *   When a std::vector goes out of scope its memory is returned to the
 *   allocator WITHOUT being erased. The bytes sit in the heap until something
 *   else happens to reuse that block. A core dump, a swap file, or a
 *   heap-spraying attacker can recover them long after the variable "died".
 *
 *   ~SecureBuffer() calls OPENSSL_cleanse(), which overwrites the memory with
 *   zeroes in a way the compiler is not permitted to optimise away. A plain
 *   memset() here would be legally removable under the as-if rule, because the
 *   compiler can prove nobody reads the buffer afterwards — this is a real and
 *   well-documented class of bug (CWE-14, "compiler removal of code to clear
 *   buffers"), not a hypothetical one.
 *
 * The type is move-only on purpose: silently copying a secret is exactly the
 * thing we are trying to prevent. Use clone() when a copy is genuinely wanted,
 * so it is visible at the call site.
 */

#pragma once

#include <openssl/crypto.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace chess {
namespace crypto {

class SecureBuffer {
public:
    SecureBuffer() = default;

    /// Allocate `size` zero-initialised bytes.
    explicit SecureBuffer(size_t size) : data_(size, 0) {}

    /// Copy `size` bytes from `data` into a new secure buffer.
    SecureBuffer(const uint8_t* data, size_t size) : data_(size) {
        if (size > 0 && data != nullptr) {
            std::memcpy(data_.data(), data, size);
        }
    }

    /// Convenience for test vectors and labels. Does NOT copy the NUL.
    static SecureBuffer from_string(const std::string& s) {
        return SecureBuffer(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    ~SecureBuffer() { wipe(); }

    // Move-only: copying a secret must be explicit (see clone()).
    SecureBuffer(const SecureBuffer&)            = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;

    SecureBuffer(SecureBuffer&& other) noexcept : data_(std::move(other.data_)) {
        other.data_.clear();
    }

    SecureBuffer& operator=(SecureBuffer&& other) noexcept {
        if (this != &other) {
            wipe();
            data_ = std::move(other.data_);
            other.data_.clear();
        }
        return *this;
    }

    /// Explicit deep copy — deliberately not a copy constructor.
    SecureBuffer clone() const { return SecureBuffer(data_.data(), data_.size()); }

    uint8_t*       data()       { return data_.data(); }
    const uint8_t* data() const { return data_.data(); }
    size_t         size() const { return data_.size(); }
    bool           empty() const { return data_.empty(); }

    uint8_t&       operator[](size_t i)       { return data_[i]; }
    const uint8_t& operator[](size_t i) const { return data_[i]; }

    /// Resize, wiping any bytes that are about to be discarded.
    void resize(size_t new_size) {
        if (new_size < data_.size()) {
            OPENSSL_cleanse(data_.data() + new_size, data_.size() - new_size);
        }
        data_.resize(new_size, 0);
    }

    /// Overwrite the contents with zeroes and release the storage.
    void wipe() {
        if (!data_.empty()) {
            OPENSSL_cleanse(data_.data(), data_.size());
            data_.clear();
        }
    }

private:
    std::vector<uint8_t> data_;
};

} // namespace crypto
} // namespace chess
