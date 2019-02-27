// Copyright Takatoshi Kondo 2016
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#if !defined(MQTT_UTILITY_HPP)
#define MQTT_UTILITY_HPP

#include <utility>
#include <memory>

#if __cplusplus >= 201402L
#define MQTT_CAPTURE_FORWARD(T, v) v = std::forward<T>(v)
#define MQTT_CAPTURE_MOVE(v) v = std::move(v)
#else
#define MQTT_CAPTURE_FORWARD(T, v) v
#define MQTT_CAPTURE_MOVE(v) v
#endif

#if __cplusplus >= 201402L
#define MQTT_DEPRECATED(msg) [[deprecated(msg)]]
#else  // __cplusplus >= 201402L
#define MQTT_DEPRECATED(msg)
#endif // __cplusplus >= 201402L


// string_view

#if __cplusplus >= 201703L

#include <string_view>

namespace mqtt {

using string_view = std::string_view;

template<class CharT, class Traits = std::char_traits<CharT>>
using basic_string_view = std::basic_string_view<CharT, Traits>;

} // namespace mqtt

#else  // __cplusplus >= 201703L

#include <boost/version.hpp>

#if !defined(MQTT_NO_BOOST_STRING_VIEW)

#if BOOST_VERSION >= 106400

#define MQTT_NO_BOOST_STRING_VIEW 0

#else  // BOOST_VERSION >= 106400

#define MQTT_NO_BOOST_STRING_VIEW 1

#endif // BOOST_VERSION >= 106400

#endif // !defined(MQTT_NO_BOOST_STRING_VIEW)


#if MQTT_NO_BOOST_STRING_VIEW

#include <boost/utility/string_ref.hpp>

#else  // MQTT_NO_BOOST_STRING_VIEW

#include <boost/utility/string_view.hpp>

#endif // MQTT_NO_BOOST_STRING_VIEW

namespace mqtt {

#if MQTT_NO_BOOST_STRING_VIEW

using string_view = boost::string_ref;

template<class CharT, class Traits = std::char_traits<CharT> >
using basic_string_view = boost::basic_string_ref<CharT, Traits>;

#else //  MQTT_NO_BOOST_STRING_VIEW

using string_view = boost::string_view;

template<class CharT, class Traits = std::char_traits<CharT> >
using basic_string_view = boost::basic_string_view<CharT, Traits>;

#endif // MQTT_NO_BOOST_STRING_VIEW

} // namespace mqtt

#endif // __cplusplus >= 201703L

// is_shared_ptr

namespace mqtt {

template <typename T>
struct is_shared_ptr {
    static constexpr bool value = false;
};

template <typename T>
struct is_shared_ptr<std::shared_ptr<T>> {
    static constexpr bool value = true;
};

} // namespace mqtt

#endif // MQTT_UTILITY_HPP
