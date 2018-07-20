// Copyright Takatoshi Kondo 2019
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#if !defined(MQTT_CONNECT_REASON_CODE_HPP)
#define MQTT_CONNECT_REASON_CODE_HPP

#include <cstdint>

namespace mqtt {

namespace connect_reason_code {

constexpr std::uint8_t const success                                = 0;
constexpr std::uint8_t const unspecified_error                      = 128;
constexpr std::uint8_t const malgormed_packet                       = 129;
constexpr std::uint8_t const protocol_error                         = 130;
constexpr std::uint8_t const implementation_specific_error          = 131;
constexpr std::uint8_t const unsupported_protocol_version           = 132;
constexpr std::uint8_t const client_identifier_not_valid            = 133;
constexpr std::uint8_t const bad_user_name_or_password              = 134;
constexpr std::uint8_t const not_authorized                         = 135;
constexpr std::uint8_t const server_unavailable                     = 136;
constexpr std::uint8_t const server_busy                            = 137;
constexpr std::uint8_t const banned                                 = 138;
constexpr std::uint8_t const bad_authentication_method              = 140;
constexpr std::uint8_t const topic_name_invalid                     = 144;
constexpr std::uint8_t const packet_too_large                       = 149;
constexpr std::uint8_t const quota_exceeded                         = 151;
constexpr std::uint8_t const payload_format_invalid                 = 153;
constexpr std::uint8_t const retain_not_supported                   = 154;
constexpr std::uint8_t const qos_not_supported                      = 155;
constexpr std::uint8_t const use_another_server                     = 156;
constexpr std::uint8_t const server_moved                           = 157;
constexpr std::uint8_t const connection_rate_exceeded               = 159;

} // namespace connect_reason_code

} // namespace mqtt

#endif // MQTT_CONNECT_REASON_CODE_HPP
