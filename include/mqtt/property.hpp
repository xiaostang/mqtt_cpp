// Copyright Takatoshi Kondo 2018
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#if !defined(MQTT_PROPERTY_HPP)
#define MQTT_PROPERTY_HPP

#include <string>
#include <vector>
#include <memory>
#include <algorithm>

#include <boost/asio/buffer.hpp>
#include <boost/optional.hpp>
#include <boost/container/static_vector.hpp>

#include <mqtt/two_byte_util.hpp>
#include <mqtt/const_buffer_util.hpp>
#include <mqtt/exception.hpp>
#include <mqtt/string_check.hpp>
#include <mqtt/property_id.hpp>
#include <mqtt/four_byte_util.hpp>
#include <mqtt/utf8encoded_strings.hpp>

namespace mqtt {

namespace as = boost::asio;

namespace v5 {

namespace property {

namespace detail {

template <std::size_t N>
struct n_bytes_property {
    explicit n_bytes_property(std::uint8_t id)
        :id_(static_cast<char>(id)) {}
    template <typename It>
    n_bytes_property(std::uint8_t id, It b, It e)
        :id_(static_cast<char>(id)), buf_(b, e) {}
    n_bytes_property(std::uint8_t id, std::initializer_list<char> il)
        :id_(static_cast<char>(id)), buf_(std::move(il)) {}

    /**
     * @brief Create const buffer sequence
     *        it is for boost asio APIs
     * @return const buffer sequence
     */
    std::vector<as::const_buffer> const_buffer_sequence() const {
        return
            {
                as::buffer(&id_, 1),
                as::buffer(buf_.data(), buf_.size())
            };
    }

    /**
     * @brief Copy the internal information to the range between b and e
     *        it is for boost asio APIs
     * @param b begin of the range to fill
     * @param e end of the range to fill
     */
    template <typename It>
    void fill(It b, It e) const {
        BOOST_ASSERT(static_cast<std::size_t>(std::distance(b, e)) >= size());
        *b++ = id_;
        std::copy(buf_.begin(), buf_.end(), b);
    }

    /**
     * @brief Get whole size of sequence
     * @return whole size
     */
    std::size_t size() const {
        return 1 + buf_.size();
    }

    char const id_;
    boost::container::static_vector<char, N> buf_;
};

struct binary_property {
    binary_property(std::uint8_t id, string_view sv)
        :id_(static_cast<char>(id)),
         length_{MQTT_16BITNUM_TO_BYTE_SEQ(sv.size())},
         buf_(sv.begin(), sv.end()) {
             if (sv.size() > 0xffff) throw property_length_error();
         }

    /**
     * @brief Create const buffer sequence
     *        it is for boost asio APIs
     * @return const buffer sequence
     */
    std::vector<as::const_buffer> const_buffer_sequence() const {
        return
            {
                as::buffer(&id_, 1),
                as::buffer(length_.data(), length_.size()),
                as::buffer(buf_.data(), buf_.size())
            };
    }

    /**
     * @brief Copy the internal information to the range between b and e
     *        it is for boost asio APIs
     * @param b begin of the range to fill
     * @param e end of the range to fill
     */
    template <typename It>
    void fill(It b, It e) const {
        BOOST_ASSERT(static_cast<std::size_t>(std::distance(b, e)) >= size());
        *b++ = id_;
        std::copy(length_.begin(), length_.end(), b);
        b += length_.size();
        std::copy(buf_.begin(), buf_.end(), b);
    }

    /**
     * @brief Get whole size of sequence
     * @return whole size
     */
    std::size_t size() const {
        return 1 + length_.size() + buf_.size();
    }

    string_view val() const {
        return buf_;
    }

    char const id_;
    boost::container::static_vector<char, 2> length_;
    std::string buf_;
};

struct binary_property_ref {
    binary_property_ref(std::uint8_t id, string_view sv)
        :id_(static_cast<char>(id)),
         length_{MQTT_16BITNUM_TO_BYTE_SEQ(sv.size())},
         buf_(sv.data(), sv.size()) {
             if (sv.size() > 0xffff) throw property_length_error();
         }

    /**
     * @brief Create const buffer sequence
     *        it is for boost asio APIs
     * @return const buffer sequence
     */
    std::vector<as::const_buffer> const_buffer_sequence() const {
        return
            {
                as::buffer(&id_, 1),
                as::buffer(length_.data(), length_.size()),
                as::buffer(get_pointer(buf_), get_size(buf_))
            };
    }

    /**
     * @brief Copy the internal information to the range between b and e
     *        it is for boost asio APIs
     * @param b begin of the range to fill
     * @param e end of the range to fill
     */
    template <typename It>
    void fill(It b, It e) const {
        BOOST_ASSERT(static_cast<std::size_t>(std::distance(b, e)) >= size());
        *b++ = id_;
        std::copy(length_.begin(), length_.end(), b);
        b += length_.size();
        std::copy(get_pointer(buf_), get_pointer(buf_) + get_size(buf_), b);
    }

    /**
     * @brief Get whole size of sequence
     * @return whole size
     */
    std::size_t size() const {
        return 1 + length_.size() + get_size(buf_);
    }

    string_view val() const {
        return string_view(get_pointer(buf_), get_size(buf_));
    }

    char const id_;
    boost::container::static_vector<char, 2> length_;
    as::const_buffer buf_;
};

struct string_property : binary_property {
    string_property(std::uint8_t id, string_view sv)
        :binary_property(id, sv) {
        auto r = utf8string::validate_contents(sv);
        if (r != utf8string::validation::well_formed) throw utf8string_contents_error(r);
    }
};

struct string_property_ref : binary_property_ref {
    string_property_ref(std::uint8_t id, string_view sv)
        :binary_property_ref(id, sv) {
        auto r = utf8string::validate_contents(sv);
        if (r != utf8string::validation::well_formed) throw utf8string_contents_error(r);
    }
};

struct variable_property {
    variable_property(std::uint8_t id, std::size_t value)
        :id_(id)  {
        variable_push(value_, value);
    }

    /**
     * @brief Create const buffer sequence
     *        it is for boost asio APIs
     * @return const buffer sequence
     */
    std::vector<as::const_buffer> const_buffer_sequence() const {
        return
            {
                as::buffer(&id_, 1),
                as::buffer(value_.data(), value_.size())
            };
    }

    /**
     * @brief Copy the internal information to the range between b and e
     *        it is for boost asio APIs
     * @param b begin of the range to fill
     * @param e end of the range to fill
     */
    template <typename It>
    void fill(It b, It e) const {
        BOOST_ASSERT(static_cast<std::size_t>(std::distance(b, e)) >= size());
        *b++ = id_;
        std::copy(value_.begin(), value_.end(), b);
    }

    /**
     * @brief Get whole size of sequence
     * @return whole size
     */
    std::size_t size() const {
        return 1 + value_.size();
    }

    std::size_t val() const {
        return std::get<0>(variable_length(value_));
    }

    char const id_;
    boost::container::static_vector<char, 4> value_;
};

} // namespace detail

class payload_format_indicator : public detail::n_bytes_property<1> {
public:
    enum payload_format {
        binary,
        string
    };

    payload_format_indicator(bool binary = true)
        : detail::n_bytes_property<1>(id::payload_format_indicator, { binary ? char(0) : char(1) } ) {}

    template <typename It>
    payload_format_indicator(It& b, It e)
        : detail::n_bytes_property<1>(id::payload_format_indicator) {
        if (b == e) throw property_parse_error();
        if (*b != 0 && *b != 1) throw property_parse_error();
        buf_[0] = *b;
        ++b;
    }

    payload_format payload_format() const {
        return
            [this] {
                if (buf_[0] == 0) return binary;
                else return string;
            }();
    }

};

class message_expiry_interval : public detail::n_bytes_property<4> {
public:
    message_expiry_interval(std::uint32_t val)
        : detail::n_bytes_property<4>(id::message_expiry_interval, { MQTT_32BITNUM_TO_BYTE_SEQ(val) } ) {}
    std::uint32_t val() const {
        return make_uint32_t(buf_.begin(), buf_.end());
    }
};

class content_type : public detail::string_property {
public:
    content_type(string_view type)
        : detail::string_property(id::content_type, type) {}
};

class content_type_ref : public detail::string_property_ref {
public:
    content_type_ref(string_view type)
        : detail::string_property_ref(id::content_type, type) {}
};

class response_topic : public detail::string_property {
public:
    response_topic(string_view type)
        : detail::string_property(id::response_topic, type) {}
};

class response_topic_ref : public detail::string_property_ref {
public:
    response_topic_ref(string_view type)
        : detail::string_property_ref(id::response_topic, type) {}
};

class correlation_data : public detail::string_property {
public:
    correlation_data(string_view type)
        : detail::string_property(id::correlation_data, type) {}
};

class correlation_data_ref : public detail::string_property_ref {
public:
    correlation_data_ref(string_view type)
        : detail::string_property_ref(id::correlation_data, type) {}
};

class subscription_identifier : public detail::variable_property {
    subscription_identifier(std::size_t subscription_id)
        : detail::variable_property(id::subscription_identifier, subscription_id) {}
};

class session_expiry_interval : public detail::n_bytes_property<4> {
    session_expiry_interval(std::uint32_t val)
        : detail::n_bytes_property<4>(id::session_expiry_interval, { MQTT_32BITNUM_TO_BYTE_SEQ(val) } ) {}
    std::uint32_t val() const {
        return make_uint32_t(buf_.begin(), buf_.end());
    }
};

class assigned_client_identifier : public detail::string_property {
public:
    assigned_client_identifier(string_view type)
        : detail::string_property(id::assigned_client_identifier, type) {}
};

class assigned_client_identifier_ref : public detail::string_property_ref {
public:
    assigned_client_identifier_ref(string_view type)
        : detail::string_property_ref(id::assigned_client_identifier, type) {}
};

class server_keep_alive : public detail::n_bytes_property<2> {
public:
    server_keep_alive(std::uint16_t val)
        : detail::n_bytes_property<2>(id::server_keep_alive, { MQTT_16BITNUM_TO_BYTE_SEQ(val) } ) {}
    std::uint16_t val() const {
        return make_uint16_t(buf_.begin(), buf_.end());
    }
};

class authentication_method : public detail::string_property {
public:
    authentication_method(string_view type)
        : detail::string_property(id::authentication_method, type) {}
};

class authentication_method_ref : public detail::string_property_ref {
public:
    authentication_method_ref(string_view type)
        : detail::string_property_ref(id::authentication_method, type) {}
};

class authentication_data : public detail::binary_property {
public:
    authentication_data(string_view type)
        : detail::binary_property(id::authentication_data, type) {}
};

class authentication_data_ref : public detail::binary_property_ref {
public:
    authentication_data_ref(string_view type)
        : detail::binary_property_ref(id::authentication_data, type) {}
};

class request_problem_information : public detail::n_bytes_property<1> {
public:
    request_problem_information(bool value)
        : detail::n_bytes_property<1>(id::request_problem_information, { value ? char(1) : char(0) } ) {}

    template <typename It>
    request_problem_information(It& b, It e)
        : detail::n_bytes_property<1>(id::request_problem_information) {
        if (b == e) throw property_parse_error();
        if (*b != 0 && *b != 1) throw property_parse_error();
        buf_[0] = *b;
        ++b;
    }

    bool val() const {
        return buf_[0] == 1;
    }
};

class will_delay_interval : public detail::n_bytes_property<4> {
    will_delay_interval(std::uint32_t val)
        : detail::n_bytes_property<4>(id::will_delay_interval, { MQTT_32BITNUM_TO_BYTE_SEQ(val) } ) {}
    std::uint32_t val() const {
        return make_uint32_t(buf_.begin(), buf_.end());
    }
};

class request_response_information : public detail::n_bytes_property<1> {
public:
    request_response_information(bool value)
        : detail::n_bytes_property<1>(id::request_response_information, { value ? char(1) : char(0) } ) {}

    template <typename It>
    request_response_information(It& b, It e)
        : detail::n_bytes_property<1>(id::request_response_information) {
        if (b == e) throw property_parse_error();
        if (*b != 0 && *b != 1) throw property_parse_error();
        buf_[0] = *b;
        ++b;
    }

    bool val() const {
        return buf_[0] == 1;
    }
};

class response_information : public detail::string_property {
public:
    response_information(string_view type)
        : detail::string_property(id::response_information, type) {}
};

class response_information_ref : public detail::string_property_ref {
public:
    response_information_ref(string_view type)
        : detail::string_property_ref(id::response_information, type) {}
};

class server_reference : public detail::string_property {
public:
    server_reference(string_view type)
        : detail::string_property(id::server_reference, type) {}
};

class server_reference_ref : public detail::string_property_ref {
public:
    server_reference_ref(string_view type)
        : detail::string_property_ref(id::server_reference, type) {}
};

class reason_string : public detail::string_property {
public:
    reason_string(string_view type)
        : detail::string_property(id::reason_string, type) {}
};

class reason_string_ref : public detail::string_property_ref {
public:
    reason_string_ref(string_view type)
        : detail::string_property_ref(id::reason_string, type) {}
};

class receive_maximum : public detail::n_bytes_property<2> {
public:
    receive_maximum(std::uint16_t val)
        : detail::n_bytes_property<2>(id::receive_maximum, { MQTT_16BITNUM_TO_BYTE_SEQ(val) } ) {}
    std::uint16_t val() const {
        return make_uint16_t(buf_.begin(), buf_.end());
    }
};

class topic_alias_maximum : public detail::n_bytes_property<2> {
public:
    topic_alias_maximum(std::uint16_t val)
        : detail::n_bytes_property<2>(id::topic_alias_maximum, { MQTT_16BITNUM_TO_BYTE_SEQ(val) } ) {}
    std::uint16_t val() const {
        return make_uint16_t(buf_.begin(), buf_.end());
    }
};

class topic_alias : public detail::n_bytes_property<2> {
public:
    topic_alias(std::uint16_t val)
        : detail::n_bytes_property<2>(id::topic_alias, { MQTT_16BITNUM_TO_BYTE_SEQ(val) } ) {}
    std::uint16_t val() const {
        return make_uint16_t(buf_.begin(), buf_.end());
    }
};

class maximum_qos : public detail::n_bytes_property<1> {
public:
    maximum_qos(std::uint8_t qos)
        : detail::n_bytes_property<1>(id::maximum_qos, { static_cast<char>(qos) } ) {
        if (qos != qos::at_most_once &&
            qos != qos::at_least_once &&
            qos != qos::exactly_once) throw property_parse_error();
    }

    template <typename It>
    maximum_qos(It& b, It e)
        : detail::n_bytes_property<1>(id::maximum_qos) {
        if (b == e) throw property_parse_error();
        if (*b != 0 && *b != 1 && *b != 2) throw property_parse_error();
        buf_[0] = *b;
        ++b;
    }

    std::uint8_t val() const {
        return static_cast<std::uint8_t>(buf_[0]);
    }
};

class retain_available : public detail::n_bytes_property<1> {
public:
    retain_available(bool value)
        : detail::n_bytes_property<1>(id::retain_available, { value ? char(1) : char(0) } ) {}

    template <typename It>
    retain_available(It& b, It e)
        : detail::n_bytes_property<1>(id::retain_available) {
        if (b == e) throw property_parse_error();
        if (*b != 0 && *b != 1) throw property_parse_error();
        buf_[0] = *b;
        ++b;
    }

    bool val() const {
        return buf_[0] == 1;
    }
};

class user_property {
    struct len_str {
        explicit len_str(string_view v)
            : len{MQTT_16BITNUM_TO_BYTE_SEQ(v.size())}
            , str(v.data(), v.size())
        {}
        std::size_t size() const {
            return len.size() + str.size();
        }
        boost::container::static_vector<char, 2> len;
        std::string str;
    };
public:
    user_property(string_view key, string_view val)
        : key_(key), val_(val) {}

    /**
     * @brief Create const buffer sequence
     *        it is for boost asio APIs
     * @return const buffer sequence
     */
    std::vector<as::const_buffer> const_buffer_sequence() const {
        std::vector<as::const_buffer> ret;
        ret.reserve(
            1 + // header
            2 + // key (len, str)
            2   // val (len, str)
        );

        ret.emplace_back(as::buffer(&id_, 1));
        ret.emplace_back(as::buffer(key_.len.data(), key_.len.size()));
        ret.emplace_back(as::buffer(key_.str.data(), key_.str.size()));
        ret.emplace_back(as::buffer(val_.len.data(), val_.len.size()));
        ret.emplace_back(as::buffer(val_.str.data(), val_.str.size()));

        return ret;
    }

    template <typename It>
    void fill(It b, It e) const {
        BOOST_ASSERT(static_cast<std::size_t>(std::distance(b, e)) >= size());

        *b++ = id_;
        {
            std::copy(key_.len.begin(), key_.len.end(), b);
            b += key_.len.size();
            auto ptr = key_.str.data();
            auto size = key_.str.size();
            std::copy(ptr, ptr + size, b);
            b += size;
        }
        {
            std::copy(val_.len.begin(), val_.len.end(), b);
            b += val_.len.size();
            auto ptr = val_.str.data();
            auto size = val_.str.size();
            std::copy(ptr, ptr + size, b);
            b += size;
        }
    }

    /**
     * @brief Get whole size of sequence
     * @return whole size
     */
    std::size_t size() const {
        return
            1 + // id_
            key_.size() +
            val_.size();
    }

private:
    char const id_ = id::user_property;
    len_str key_;
    len_str val_;
};

class user_property_ref {
    struct len_str {
        explicit len_str(string_view v)
            : len{MQTT_16BITNUM_TO_BYTE_SEQ(v.size())}
            , str(as::buffer(v.data(), v.size()))
        {}
#if 0
        explicit len_str(as::const_buffer v)
            : len{MQTT_16BITNUM_TO_BYTE_SEQ(get_size(v))}
            , str(v)
        {}
#endif
        std::size_t size() const {
            return len.size() + get_size(str);
        }
        boost::container::static_vector<char, 2> len;
        as::const_buffer str;
    };
public:
    user_property_ref(string_view key, string_view val)
        : key_(key), val_(val) {}

    /**
     * @brief Create const buffer sequence
     *        it is for boost asio APIs
     * @return const buffer sequence
     */
    std::vector<as::const_buffer> const_buffer_sequence() const {
        std::vector<as::const_buffer> ret;
        ret.reserve(
            1 + // header
            2 + // key (len, str)
            2   // val (len, str)
        );

        ret.emplace_back(as::buffer(&id_, 1));
        ret.emplace_back(as::buffer(key_.len.data(), key_.len.size()));
        ret.emplace_back(key_.str);
        ret.emplace_back(as::buffer(val_.len.data(), val_.len.size()));
        ret.emplace_back(val_.str);

        return ret;
    }

    template <typename It>
    void fill(It b, It e) const {
        BOOST_ASSERT(static_cast<std::size_t>(std::distance(b, e)) >= size());

        *b++ = id_;
        {
            std::copy(key_.len.begin(), key_.len.end(), b);
            b += key_.len.size();
            auto ptr = get_pointer(key_.str);
            auto size = get_size(key_.str);
            std::copy(ptr, ptr + size, b);
            b += size;
        }
        {
            std::copy(val_.len.begin(), val_.len.end(), b);
            b += val_.len.size();
            auto ptr = get_pointer(val_.str);
            auto size = get_size(val_.str);
            std::copy(ptr, ptr + size, b);
            b += size;
        }
    }

    /**
     * @brief Get whole size of sequence
     * @return whole size
     */
    std::size_t size() const {
        return
            1 + // id_
            key_.size() +
            val_.size();
    }

private:
    char const id_ = id::user_property;
    len_str key_;
    len_str val_;
};

class maximum_packet_size : public detail::n_bytes_property<4> {
    maximum_packet_size(std::uint32_t val)
        : detail::n_bytes_property<4>(id::maximum_packet_size, { MQTT_32BITNUM_TO_BYTE_SEQ(val) } ) {}
    std::uint32_t val() const {
        return make_uint32_t(buf_.begin(), buf_.end());
    }
};

class wildcard_subscription_available : public detail::n_bytes_property<1> {
public:
    wildcard_subscription_available(bool value)
        : detail::n_bytes_property<1>(id::wildcard_subscription_available, { value ? char(1) : char(0) } ) {}

    template <typename It>
    wildcard_subscription_available(It& b, It e)
        : detail::n_bytes_property<1>(id::wildcard_subscription_available) {
        if (b == e) throw property_parse_error();
        if (*b != 0 && *b != 1) throw property_parse_error();
        buf_[0] = *b;
        ++b;
    }

    bool val() const {
        return buf_[0] == 1;
    }
};

class subscription_identifier_available : public detail::n_bytes_property<1> {
public:
    subscription_identifier_available(bool value)
        : detail::n_bytes_property<1>(id::subscription_identifier_available, { value ? char(1) : char(0) } ) {}

    template <typename It>
    subscription_identifier_available(It& b, It e)
        : detail::n_bytes_property<1>(id::subscription_identifier_available) {
        if (b == e) throw property_parse_error();
        if (*b != 0 && *b != 1) throw property_parse_error();
        buf_[0] = *b;
        ++b;
    }

    bool val() const {
        return buf_[0] == 1;
    }
};

class shared_subscription_available : public detail::n_bytes_property<1> {
public:
    shared_subscription_available(bool value)
        : detail::n_bytes_property<1>(id::shared_subscription_available, { value ? char(1) : char(0) } ) {}

    template <typename It>
    shared_subscription_available(It& b, It e)
        : detail::n_bytes_property<1>(id::shared_subscription_available) {
        if (b == e) throw property_parse_error();
        if (*b != 0 && *b != 1) throw property_parse_error();
        buf_[0] = *b;
        ++b;
    }

    bool val() const {
        return buf_[0] == 1;
    }
};

} // namespace property
} // namespace v5
} // namespace mqtt

#endif // MQTT_PROPERTY_HPP
