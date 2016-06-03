/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef EXPIRY_HXX
#define EXPIRY_HXX

#include "system/clock.h"

#include <limits>

/**
 * Helper library for handling expiry time stamps using the system's
 * monotonic clock.
 */
class Expiry {
    typedef decltype(now_s()) value_type;
    value_type value;

    constexpr Expiry(value_type _value):value(_value) {}

public:
    Expiry() = default;

    static Expiry Now() {
        return now_s();
    }

    static Expiry AlreadyExpired() {
        return std::numeric_limits<value_type>::min();
    }

    static Expiry Never() {
        return std::numeric_limits<value_type>::max();
    }

    static constexpr Expiry Touched(Expiry now, unsigned duration) {
        return now.value + duration;
    }

    static Expiry Touched(unsigned duration) {
        return Touched(Now(), duration);
    }

    void Touch(Expiry now, unsigned duration) {
        value = now.value + duration;
    }

    void Touch(unsigned duration) {
        Touch(Now(), duration);
    }

    constexpr bool IsExpired(Expiry now) const {
        return now >= *this;
    }

    bool IsExpired() const {
        return IsExpired(Now());
    }

    constexpr bool operator==(Expiry other) const {
        return value == other.value;
    }

    constexpr bool operator>=(Expiry other) const {
        return value >= other.value;
    }

    static constexpr Expiry Import(value_type value) {
        return value;
    }

    constexpr value_type Export() const {
        return value;
    }
};

#endif
