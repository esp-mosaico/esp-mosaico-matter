/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "cJSON.h"

#include <atomic>
#include <new>
#include <utility>

namespace data_model {

// Immutable variable-length adapter data shared by reports and committed state.
class shared_json {
  public:
    shared_json() = default;
    shared_json(const shared_json &other) : value_(other.value_)
    {
        if (value_) value_->references.fetch_add(1, std::memory_order_relaxed);
    }
    shared_json(shared_json &&other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}
    shared_json &operator=(const shared_json &other)
    {
        shared_json replacement(other);
        std::swap(value_, replacement.value_);
        return *this;
    }
    shared_json &operator=(shared_json &&other) noexcept
    {
        shared_json replacement(std::move(other));
        std::swap(value_, replacement.value_);
        return *this;
    }
    ~shared_json() { release(); }

    const cJSON *get() const { return value_ ? value_->json : nullptr; }

    // Takes ownership even on failure; allocation failure preserves this value.
    bool reset(cJSON *json)
    {
        storage *replacement = json ? new (std::nothrow) storage(json) : nullptr;
        if (json && !replacement) {
            cJSON_Delete(json);
            return false;
        }
        release();
        value_ = replacement;
        return true;
    }

  private:
    struct storage {
        explicit storage(cJSON *value) : json(value) {}
        std::atomic<size_t> references{1};
        cJSON *json;
    };

    void release()
    {
        if (value_ &&
            value_->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            cJSON_Delete(value_->json);
            delete value_;
        }
        value_ = nullptr;
    }

    storage *value_ = nullptr;
};

} // namespace data_model
