// Copyright Takatoshi Kondo 2018
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#if !defined(MQTT_PROPERTY_PARSE_HPP)
#define MQTT_PROPERTY_PARSE_HPP

#include <vector>
#include <boost/optional.hpp>

#include <mqtt/property_variant.hpp>

namespace mqtt {
namespace v5 {
namespace property {

inline
boost::optional<property_variant> parse_one(char const*& it, char const* end) {
    if (it == end) return boost::none;
    try {
        switch (static_cast<std::uint8_t>(*it)) {
        case id::payload_format_indicator:
            if (it + 1 >= end) return boost::none;
            return property_variant(payload_format_indicator(++it, end));
        case id::message_expiry_interval:
            break;
        case id::content_type:
            break;
        case id::response_topic:
            break;
        case id::correlation_data:
            break;
        case id::subscription_identifier:
            break;
        case id::session_expiry_interval:
            break;
        case id::assigned_client_identifier:
            break;
        case id::server_keep_alive:
            break;
        case id::authentication_method:
            break;
        case id::request_problem_information:
            break;
        case id::will_delay_interval:
            break;
        case id::request_response_information:
            break;
        case id::response_information:
            break;
        case id::server_reference:
            break;
        case id::reason_string:
            break;
        case id::receive_maximum:
            break;
        case id::topic_alias_maximum:
            break;
        case id::topic_alias:
            break;
        case id::maximum_qos:
            break;
        case id::retain_available:
            break;
        case id::user_property:
            break;
        case id::maximum_packet_size:
            break;
        case id::wildcard_subscription_available:
            break;
        case id::subscription_identifier_available:
            break;
        case id::shared_subscription_available:
            break;
        }
    }
    catch (property_parse_error const&) {
        return boost::none;
    }
    return boost::none;
}

inline
std::vector<property_variant> parse(char const*& it, char const* end) {
    std::vector<property_variant> props;
    while (true) {
        if (auto p = parse_one(it, end)) {
            props.push_back(std::move(*p));
        }
        else {
            break;
        }
    }
    return props;
}

inline
boost::optional<std::vector<property_variant>> parse_with_length(char const*& it, char const* end) {
    auto r = variable_length(
        it,
        std::min(it + 4, end)
    );
    auto property_length = std::get<0>(r);
    it += std::get<1>(r);

    if (end < it + property_length) {
        return boost::none;
    }

    char const* prop_end = it +  property_length;
    return v5::property::parse(it, prop_end);
}

} // namespace property
} // namespace v5
} // namespace mqtt

#endif // MQTT_PROPERTY_PARSE_HPP
