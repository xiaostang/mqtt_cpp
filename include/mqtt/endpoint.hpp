// Copyright Takatoshi Kondo 2015
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#if !defined(MQTT_ENDPOINT_HPP)
#define MQTT_ENDPOINT_HPP

#include <mqtt/variant.hpp> // should be top to configure variant limit

#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <set>
#include <memory>
#include <mutex>
#include <atomic>

#include <boost/any.hpp>
#include <boost/optional.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/composite_key.hpp>
#include <boost/system/error_code.hpp>
#include <boost/assert.hpp>

#include <mqtt/fixed_header.hpp>
#include <mqtt/remaining_length.hpp>
#include <mqtt/utf8encoded_strings.hpp>
#include <mqtt/connect_flags.hpp>
#include <mqtt/encoded_length.hpp>
#include <mqtt/will.hpp>
#include <mqtt/session_present.hpp>
#include <mqtt/qos.hpp>
#include <mqtt/publish.hpp>
#include <mqtt/connect_return_code.hpp>
#include <mqtt/exception.hpp>
#include <mqtt/tcp_endpoint.hpp>
#include <mqtt/unique_scope_guard.hpp>
#include <mqtt/shared_scope_guard.hpp>
#include <mqtt/message_variant.hpp>
#include <mqtt/two_byte_util.hpp>
#include <mqtt/four_byte_util.hpp>
#include <mqtt/packet_id_type.hpp>
#include <mqtt/property_variant.hpp>
#include <mqtt/property_parse.hpp>
#include <mqtt/protocol_version.hpp>

#if defined(MQTT_USE_WS)
#include <mqtt/ws_endpoint.hpp>
#endif // defined(MQTT_USE_WS)

namespace mqtt {

namespace as = boost::asio;
namespace mi = boost::multi_index;

template <typename Socket, typename Mutex = std::mutex, template<typename...> class LockGuard = std::lock_guard, std::size_t PacketIdBytes = 2>
class endpoint : public std::enable_shared_from_this<endpoint<Socket, Mutex, LockGuard, PacketIdBytes>> {
    using this_type = endpoint<Socket, Mutex, LockGuard, PacketIdBytes>;
public:
    using async_handler_t = std::function<void(boost::system::error_code const& ec)>;
    using life_keeper_t = std::function<void()>;
    using packet_id_t = typename packet_id_type<PacketIdBytes>::type;

    /**
     * @brief Constructor for client
     */
    endpoint(std::uint8_t version = protocol_version::undetermined)
        :connected_(false),
         mqtt_connected_(false),
         clean_session_(false),
         packet_id_master_(0),
         auto_pub_response_(true),
         auto_pub_response_async_(false),
         disconnect_requested_(false),
         connect_requested_(false),
         h_mqtt_message_processed_(
             [this]
             (async_handler_t const& func) {
                 async_read_control_packet_type(func);
             }
         ),
         version_(version)
    {}

    /**
     * @brief Constructor for server.
     *        socket should have already been connected with another endpoint.
     */
    endpoint(std::unique_ptr<Socket>&& socket, std::uint8_t version = protocol_version::undetermined)
        :socket_(std::move(socket)),
         connected_(true),
         mqtt_connected_(false),
         clean_session_(false),
         packet_id_master_(0),
         auto_pub_response_(true),
         auto_pub_response_async_(false),
         disconnect_requested_(false),
         connect_requested_(false),
         h_mqtt_message_processed_(
             [this]
             (async_handler_t const& func) {
                 async_read_control_packet_type(func);
             }
         ),
         version_(version)
    {}

    // MQTT Common handlers

    /**
     * @brief Pingreq handler
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718081<BR>
     *        3.13 PINGREQ – PING request
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using pingreq_handler = std::function<bool()>;

    /**
     * @brief Pingresp handler
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718086<BR>
     *        3.13 PINGRESP – PING response
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using pingresp_handler = std::function<bool()>;


    // MQTT v3_1_1 handlers

    /**
     * @brief Connect handler
     * @param client_id
     *        User Name.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349245<BR>
     *        3.1.3.4 User Name
     * @param username
     *        User Name.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349245<BR>
     *        3.1.3.4 User Name
     * @param password
     *        Password.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349246<BR>
     *        3.1.3.5 Password
     * @param will
     *        Will. It contains retain, QoS, topic, and message.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349232<BR>
     *        3.1.2.5 Will Flag<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349233<BR>
     *        3.1.2.6 Will QoS<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349234<BR>
     *        3.1.2.7 Will Retain<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349243<BR>
     *        3.1.3.2 Will Topic<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349244<BR>
     *        3.1.3.3 Will Message<BR>
     * @param clean_session
     *        Clean Session<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349231<BR>
     *        3.1.2.4 Clean Session
     * @param keep_alive
     *        Keep Alive<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349237<BR>
     *        3.1.2.10 Keep Alive
     * @return if the handler returns true, then continue receiving, otherwise quit.
     *
     */
    using connect_handler = std::function<
        bool(std::string const& client_id,
             boost::optional<std::string> const& username,
             boost::optional<std::string> const& password,
             boost::optional<will> will,
             bool clean_session,
             std::uint16_t keep_alive)>;

    /**
     * @brief Connack handler
     * @param session_present
     *        Session present flag.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718035<BR>
     *        3.2.2.2 Session Present
     * @param return_code
     *        connect_return_code<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718035<BR>
     *        3.2.2.3 Connect Return code
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using connack_handler = std::function<bool(bool session_present, std::uint8_t return_code)>;

    /**
     * @brief Publish handler
     * @param fixed_header
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1 Fixed header<BR>
     *        You can check the fixed header using mqtt::publish functions.
     * @param packet_id
     *        packet identifier<BR>
     *        If received publish's QoS is 0, packet_id is boost::none.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718039<BR>
     *        3.3.2  Variable header
     * @param topic_name
     *        Topic name
     * @param contents
     *        Published contents
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using publish_handler = std::function<bool(std::uint8_t fixed_header,
                                               boost::optional<packet_id_t> packet_id,
                                               std::string topic_name,
                                               std::string contents)>;

    /**
     * @brief Puback handler
     * @param packet_id
     *        packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718045<BR>
     *        3.4.2 Variable header
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using puback_handler = std::function<bool(packet_id_t packet_id)>;

    /**
     * @brief Pubrec handler
     * @param packet_id
     *        packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718050<BR>
     *        3.5.2 Variable header
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using pubrec_handler = std::function<bool(packet_id_t packet_id)>;

    /**
     * @brief Pubrel handler
     * @param packet_id
     *        packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349791<BR>
     *        3.6.2 Variable header
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using pubrel_handler = std::function<bool(packet_id_t packet_id)>;

    /**
     * @brief Pubcomp handler
     * @param packet_id
     *        packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718060<BR>
     *        3.7.2 Variable header
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using pubcomp_handler = std::function<bool(packet_id_t packet_id)>;

    /**
     * @brief Subscribe handler
     * @param packet_id packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349801<BR>
     *        3.8.2 Variable header
     * @param entries
     *        Collection of a pair of Topic Filter and QoS.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349802<BR>
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using subscribe_handler = std::function<bool(packet_id_t packet_id,
                                                 std::vector<std::tuple<std::string, std::uint8_t>> entries)>;

    /**
     * @brief Suback handler
     * @param packet_id packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718070<BR>
     *        3.9.2 Variable header
     * @param qoss
     *        Collection of QoS that is corresponding to subscribed topic order.<BR>
     *        If subscription is failure, the value is boost::none.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718071<BR>
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using suback_handler = std::function<bool(packet_id_t packet_id,
                                              std::vector<boost::optional<std::uint8_t>> qoss)>;

    /**
     * @brief Unsubscribe handler
     * @param packet_id packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349810<BR>
     *        3.10.2 Variable header
     * @param topics
     *        Collection of Topic Filters<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc384800448<BR>
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using unsubscribe_handler = std::function<bool(packet_id_t packet_id,
                                                   std::vector<std::string> topics)>;

    /**
     * @brief Unsuback handler
     * @param packet_id packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718045<BR>
     *        3.11.2 Variable header
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using unsuback_handler = std::function<bool(packet_id_t)>;

    /**
     * @brief Disconnect handler
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc384800463<BR>
     *        3.14 DISCONNECT – Disconnect notification
     */
    using disconnect_handler = std::function<void()>;

    // MQTT v5 handlers

    /**
     * @brief Connect handler
     * @param client_id
     *        User Name.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349245<BR>
     *        3.1.3.4 User Name
     * @param username
     *        User Name.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349245<BR>
     *        3.1.3.4 User Name
     * @param password
     *        Password.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349246<BR>
     *        3.1.3.5 Password
     * @param will
     *        Will. It contains retain, QoS, topic, and message.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349232<BR>
     *        3.1.2.5 Will Flag<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349233<BR>
     *        3.1.2.6 Will QoS<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349234<BR>
     *        3.1.2.7 Will Retain<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349243<BR>
     *        3.1.3.2 Will Topic<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349244<BR>
     *        3.1.3.3 Will Message<BR>
     * @param clean_session
     *        Clean Session<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349231<BR>
     *        3.1.2.4 Clean Session
     * @param keep_alive
     *        Keep Alive<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349237<BR>
     *        3.1.2.10 Keep Alive
     * @param props
     *        Properties
     * @return if the handler returns true, then continue receiving, otherwise quit.
     *
     */
    using v5_connect_handler = std::function<
        bool(std::string const& client_id,
             boost::optional<std::string> const& username,
             boost::optional<std::string> const& password,
             boost::optional<will> will,
             bool clean_session,
             std::uint16_t keep_alive,
             std::vector<v5::property_variant> props)
    >;

    /**
     * @brief Connack handler
     * @param session_present
     *        Session present flag.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718035<BR>
     *        3.2.2.2 Session Present
     * @param reason_code
     *        CONNACK Reason Code<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718035<BR>
     *        3.2.2.3 Connect Reason code
     * @param props
     *        Properties
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using v5_connack_handler = std::function<
        bool(bool session_present,
             std::uint8_t reason_code,
             std::vector<v5::property_variant> props)
    >;

    /**
     * @brief Publish handler
     * @param fixed_header
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1 Fixed header<BR>
     *        You can check the fixed header using mqtt::publish functions.
     * @param packet_id
     *        packet identifier<BR>
     *        If received publish's QoS is 0, packet_id is boost::none.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718039<BR>
     *        3.3.2  Variable header
     * @param topic_name
     *        Topic name
     * @param contents
     *        Published contents
     * @param props
     *        Properties
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using v5_publish_handler = std::function<
        bool(std::uint8_t fixed_header,
             boost::optional<packet_id_t> packet_id,
             std::string topic_name,
             std::string contents,
             std::vector<v5::property_variant> props)
    >;

    /**
     * @brief Puback handler
     * @param packet_id
     *        packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718045<BR>
     *        3.4.2 Variable header
     * @param reason_code
     *        PUBACK Reason Code<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718035<BR>
     *        3.2.2.3 Connect Reason code
     * @param props
     *        Properties
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using v5_puback_handler = std::function<
        bool(packet_id_t packet_id,
             std::uint8_t reason_code,
             std::vector<v5::property_variant> props)
    >;

    /**
     * @brief Pubrec handler
     * @param packet_id
     *        packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718050<BR>
     *        3.5.2 Variable header
     * @param reason_code
     *        PUBREC Reason Code<BR>
     * @param props
     *        Properties
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using v5_pubrec_handler = std::function<
        bool(packet_id_t packet_id,
             std::uint8_t reason_code,
             std::vector<v5::property_variant> props)
    >;

    /**
     * @brief Pubrel handler
     * @param packet_id
     *        packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349791<BR>
     *        3.6.2 Variable header
     * @param reason_code
     *        PUBREL Reason Code<BR>
     * @param props
     *        Properties
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using v5_pubrel_handler = std::function<
        bool(packet_id_t packet_id,
             std::uint8_t reason_code,
             std::vector<v5::property_variant> props)
    >;

    /**
     * @brief Pubcomp handler
     * @param packet_id
     *        packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718060<BR>
     *        3.7.2 Variable header
     * @param reason_code
     *        PUBCOMP Reason Code<BR>
     * @param props
     *        Properties
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using v5_pubcomp_handler = std::function<
        bool(packet_id_t packet_id,
             std::uint8_t reason_code,
             std::vector<v5::property_variant> props)
    >;
    /**
     * @brief Subscribe handler
     * @param packet_id packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349801<BR>
     *        3.8.2 Variable header
     * @param entries
     *        Collection of a pair of Topic Filter and QoS.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349802<BR>
     * @param props
     *        Properties
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using v5_subscribe_handler = std::function<
        bool(packet_id_t packet_id,
             std::vector<std::tuple<std::string, std::uint8_t>> entries,
             std::vector<v5::property_variant> props)
    >;

    /**
     * @brief Suback handler
     * @param packet_id packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718070<BR>
     *        3.9.2 Variable header
     * @param reasons
     *        Collection of reason_code.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718071<BR>
     * @param props
     *        Properties
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using v5_suback_handler = std::function<
        bool(packet_id_t packet_id,
             std::vector<std::uint8_t> reasons,
             std::vector<v5::property_variant> props)
    >;

    /**
     * @brief Unsubscribe handler
     * @param packet_id packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349810<BR>
     *        3.10.2 Variable header
     * @param topics
     *        Collection of Topic Filters<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc384800448<BR>
     * @param props
     *        Properties
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using v5_unsubscribe_handler = std::function<
        bool(packet_id_t packet_id,
             std::vector<std::string> topics,
             std::vector<v5::property_variant> props)
    >;

    /**
     * @brief Unsuback handler
     * @param packet_id packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718045<BR>
     *        3.11.2 Variable header
     * @param reasons
     *        Collection of reason_code.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718071<BR>
     * @param props
     *        Properties
     * @return if the handler returns true, then continue receiving, otherwise quit.
     */
    using v5_unsuback_handler = std::function<
        bool(packet_id_t,
             std::vector<std::uint8_t> reasons,
             std::vector<v5::property_variant> props)
    >;

    /**
     * @brief Disconnect handler
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc384800463<BR>
     *        3.14 DISCONNECT – Disconnect notification
     * @param reason_code
     *        DISCONNECT Reason Code<BR>
     * @param props
     *        Properties
     */
    using v5_disconnect_handler = std::function<
        void(std::uint8_t reason_code,
             std::vector<v5::property_variant> props)
    >;

    /**
     * @brief Auth handler
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc384800463<BR>
     *        3.15 AUTH – Authentication exchange
     * @param reason_code
     *        AUTH Reason Code<BR>
     * @param props
     *        Properties
     */
    using v5_auth_handler = std::function<
        void(std::uint8_t reason_code,
             std::vector<v5::property_variant> props)
    >;


    // Original handlers

    /**
     * @brief Close handler
     *
     * This handler is called if the client called `disconnect()` and the server closed the socket cleanly.
     * If the socket is closed by other reasons, error_handler is called.
     */
    using close_handler = std::function<void()>;

    /**
     * @brief Error handler
     *
     * This handler is called if the socket is closed without client's `disconnect()` call.
     *
     * @param ec error code
     */
    using error_handler = std::function<void(boost::system::error_code const& ec)>;

    /**
     * @brief Publish response sent handler
     *        This function is called just after puback sent on QoS1, or pubcomp sent on QoS2.
     * @param packet_id
     *        packet identifier<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718060<BR>
     *        3.7.2 Variable header
     */
    using pub_res_sent_handler = std::function<void(packet_id_t packet_id)>;

    /**
     * @brief Serialize publish handler
     *        You can serialize the publish message.
     *        To restore the message, use restore_serialized_message().
     * @param msg publish message
     */
    using serialize_publish_message_handler = std::function<void(basic_publish_message<PacketIdBytes> msg)>;

    /**
     * @brief Serialize publish handler
     *        You can serialize the publish message.
     *        To restore the message, use restore_serialized_message().
     * @param msg v5::publish message
     */
    using serialize_v5_publish_message_handler = std::function<void(v5::basic_publish_message<PacketIdBytes> msg)>;

    /**
     * @brief Serialize publish handler
     *        You can serialize the publish message.
     *        To restore the message, use restore_serialized_message().
     * @param packet_id packet identifier of the serializing message
     * @param data      pointer to the serializing message
     * @param size      size of the serializing message
     */
    using serialize_publish_handler = std::function<void(packet_id_t packet_id, char const* data, std::size_t size)>;

    /**
     * @brief Serialize pubrel handler
     *        You can serialize the pubrel message.
     *        If your storage has already had the publish message that has the same packet_id,
     *        then you need to replace the publish message to pubrel message.
     *        To restore the message, use restore_serialized_message().
     * @param msg pubrel message
     */
    using serialize_pubrel_message_handler = std::function<void(basic_pubrel_message<PacketIdBytes> msg)>;

    /**
     * @brief Serialize pubrel handler
     *        You can serialize the pubrel message.
     *        If your storage has already had the publish message that has the same packet_id,
     *        then you need to replace the publish message to pubrel message.
     *        To restore the message, use restore_serialized_message().
     * @param msg pubrel message
     */
    using serialize_v5_pubrel_message_handler = std::function<void(v5::basic_pubrel_message<PacketIdBytes> msg)>;

    /**
     * @brief Serialize pubrel handler
     *        You can serialize the pubrel message.
     *        If your storage has already had the publish message that has the same packet_id,
     *        then you need to replace the publish message to pubrel message.
     *        To restore the message, use restore_serialized_message().
     * @param packet_id packet identifier of the serializing message
     * @param data      pointer to the serializing message
     * @param size      size of the serializing message
     */
    using serialize_pubrel_handler = std::function<void(packet_id_t packet_id, char const* data, std::size_t size)>;

    /**
     * @brief Remove serialized message
     * @param packet_id packet identifier of the removing message
     */
    using serialize_remove_handler = std::function<void(packet_id_t packet_id)>;

    /**
     * @brief Pre-send handler
     *        This handler is called when any mqtt control packet is decided to send.
     */
    using pre_send_handler = std::function<void()>;

    /**
     * @brief is valid length handler
     *        This handler is called when remaining length is received.
     * @param control_packet_type control_packet_type that has variable length
     * @param remaining length
     * @return true if check is success, otherwise false
     */
    using is_valid_length_handler =
        std::function<bool(std::uint8_t control_packet_type, std::size_t remaining_length)>;

    /**
     * @brief next read handler
     *        This handler is called when the current mqtt message has been processed.
     * @param func A callback function that is called when async operation will finish.
     */
    using mqtt_message_processed_handler =
        std::function<void(async_handler_t const& func)>;

    endpoint(this_type const&) = delete;
    endpoint(this_type&&) = delete;
    endpoint& operator=(this_type const&) = delete;
    endpoint& operator=(this_type&&) = delete;

    /**
     * @brief Set client id.
     * @param id client id
     *
     * This function should be called before calling connect().<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718031<BR>
     * 3.1.3.1 Client Identifier
     */
    void set_client_id(std::string id) {
        client_id_ = std::move(id);
    }

    /**
     * @brief Get client id.
     *
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718031<BR>
     * 3.1.3.1 Client Identifier
     * @return client id
     */
    std::string const& client_id() const {
        return client_id_;
    }

    /**
     * @brief Set clean session.
     * @param cs clean session
     *
     * This function should be called before calling connect().<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718029<BR>
     * 3.1.2.4 Clean Session<BR>
     * After constructing a endpoint, the clean session is set to false.
     */
    void set_clean_session(bool cs) {
        clean_session_ = cs;
    }

    /**
     * @brief Get clean session.
     *
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718029<BR>
     * 3.1.2.4 Clean Session<BR>
     * After constructing a endpoint, the clean session is set to false.
     * @return clean session
     */
    bool clean_session() const {
        return clean_session_;
    }

    /**
     * @brief Set username.
     * @param name username
     *
     * This function should be called before calling connect().<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718031<BR>
     * 3.1.3.4 User Name
     */
    void set_user_name(std::string name) {
        user_name_ = std::move(name);
    }

    /**
     * @brief Set password.
     * @param password password
     *
     * This function should be called before calling connect().<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718031<BR>
     * 3.1.3.5 Password
     */
    void set_password(std::string password) {
        password_ = std::move(password);
    }

    /**
     * @brief Set will.
     * @param w will
     *
     * This function should be called before calling connect().<BR>
     * 'will' would be send when endpoint is disconnected without calling disconnect().
     */
    void set_will(will w) {
        will_ = std::move(w);
    }

    /**
     * @brief Set auto publish response mode.
     * @param b set value
     * @param async auto publish ressponse send asynchronous
     *
     * When set auto publish response mode to true, puback, pubrec, pubrel,and pub comp automatically send.<BR>
     */
    void set_auto_pub_response(bool b = true, bool async = true) {
        auto_pub_response_ = b;
        auto_pub_response_async_ = async;
    }


    // MQTT Common handlers

    /**
     * @brief Set pingreq handler
     * @param h handler
     */
    void set_pingreq_handler(pingreq_handler h = pingreq_handler()) {
        h_pingreq_ = std::move(h);
    }

    /**
     * @brief Set pingresp handler
     * @param h handler
     */
    void set_pingresp_handler(pingresp_handler h = pingresp_handler()) {
        h_pingresp_ = std::move(h);
    }


    /**
     * @brief Get pingreq handler
     * @return handler
     */
    pingreq_handler get_pingreq_handler() const {
        return h_pingreq_;
    }

    /**
     * @brief Get pingresp handler
     * @return handler
     */
    pingresp_handler get_pingresp_handler() const {
        return h_pingresp_;
    }


    // MQTT v3_1_1 handlers

    /**
     * @brief Set connect handler
     * @param h handler
     */
    void set_connect_handler(connect_handler h = connect_handler()) {
        h_connect_ = std::move(h);
    }

    /**
     * @brief Set connack handler
     * @param h handler
     */
    void set_connack_handler(connack_handler h = connack_handler()) {
        h_connack_ = std::move(h);
    }

    /**
     * @brief Set publish handler
     * @param h handler
     */
    void set_publish_handler(publish_handler h = publish_handler()) {
        h_publish_ = std::move(h);
    }

    /**
     * @brief Set puback handler
     * @param h handler
     */
    void set_puback_handler(puback_handler h = puback_handler()) {
        h_puback_ = std::move(h);
    }

    /**
     * @brief Set pubrec handler
     * @param h handler
     */
    void set_pubrec_handler(pubrec_handler h = pubrec_handler()) {
        h_pubrec_ = std::move(h);
    }

    /**
     * @brief Set pubrel handler
     * @param h handler
     */
    void set_pubrel_handler(pubrel_handler h = pubrel_handler()) {
        h_pubrel_ = std::move(h);
    }

    /**
     * @brief Set pubcomp handler
     * @param h handler
     */
    void set_pubcomp_handler(pubcomp_handler h = pubcomp_handler()) {
        h_pubcomp_ = std::move(h);
    }

    /**
     * @brief Set subscribe handler
     * @param h handler
     */
    void set_subscribe_handler(subscribe_handler h = subscribe_handler()) {
        h_subscribe_ = std::move(h);
    }

    /**
     * @brief Set suback handler
     * @param h handler
     */
    void set_suback_handler(suback_handler h = suback_handler()) {
        h_suback_ = std::move(h);
    }

    /**
     * @brief Set unsubscribe handler
     * @param h handler
     */
    void set_unsubscribe_handler(unsubscribe_handler h = unsubscribe_handler()) {
        h_unsubscribe_ = std::move(h);
    }

    /**
     * @brief Set unsuback handler
     * @param h handler
     */
    void set_unsuback_handler(unsuback_handler h = unsuback_handler()) {
        h_unsuback_ = std::move(h);
    }

    /**
     * @brief Set disconnect handler
     * @param h handler
     */
    void set_disconnect_handler(disconnect_handler h = disconnect_handler()) {
        h_disconnect_ = std::move(h);
    }

    /**
     * @brief Get connect handler
     * @return handler
     */
    connect_handler get_connect_handler() const {
        return h_connect_;
    }

    /**
     * @brief Get connack handler
     * @return handler
     */
    connack_handler get_connack_handler() const {
        return h_connack_;
    }

    /**
     * @brief Set publish handler
     * @return handler
     */
    publish_handler get_publish_handler() const {
        return h_publish_;
    }

    /**
     * @brief Get puback handler
     * @return handler
     */
    puback_handler get_puback_handler() const {
        return h_puback_;
    }

    /**
     * @brief Get pubrec handler
     * @return handler
     */
    pubrec_handler get_pubrec_handler() const {
        return h_pubrec_;
    }

    /**
     * @brief Get pubrel handler
     * @return handler
     */
    pubrel_handler get_pubrel_handler() const {
        return h_pubrel_;
    }

    /**
     * @brief Get pubcomp handler
     * @return handler
     */
    pubcomp_handler get_pubcomp_handler() const {
        return h_pubcomp_;
    }

    /**
     * @brief Get subscribe handler
     * @return handler
     */
    subscribe_handler get_subscribe_handler() const {
        return h_subscribe_;
    }

    /**
     * @brief Get suback handler
     * @return handler
     */
    suback_handler get_suback_handler() const {
        return h_suback_;
    }

    /**
     * @brief Get unsubscribe handler
     * @return handler
     */
    unsubscribe_handler get_unsubscribe_handler() const {
        return h_unsubscribe_;
    }

    /**
     * @brief Get unsuback handler
     * @return handler
     */
    unsuback_handler get_unsuback_handler() const {
        return h_unsuback_;
    }

    /**
     * @brief Get disconnect handler
     * @return handler
     */
    disconnect_handler get_disconnect_handler() const {
        return h_disconnect_;
    }

    // MQTT v5 handlers

    /**
     * @brief Set connect handler
     * @param h handler
     */
    void set_v5_connect_handler(v5_connect_handler h = v5_connect_handler()) {
        h_v5_connect_ = std::move(h);
    }

    /**
     * @brief Set connack handler
     * @param h handler
     */
    void set_v5_connack_handler(v5_connack_handler h = v5_connack_handler()) {
        h_v5_connack_ = std::move(h);
    }

    /**
     * @brief Set publish handler
     * @param h handler
     */
    void set_v5_publish_handler(v5_publish_handler h = v5_publish_handler()) {
        h_v5_publish_ = std::move(h);
    }

    /**
     * @brief Set puback handler
     * @param h handler
     */
    void set_v5_puback_handler(v5_puback_handler h = v5_puback_handler()) {
        h_v5_puback_ = std::move(h);
    }

    /**
     * @brief Set pubrec handler
     * @param h handler
     */
    void set_v5_pubrec_handler(v5_pubrec_handler h = v5_pubrec_handler()) {
        h_v5_pubrec_ = std::move(h);
    }

    /**
     * @brief Set pubrel handler
     * @param h handler
     */
    void set_v5_pubrel_handler(v5_pubrel_handler h = v5_pubrel_handler()) {
        h_v5_pubrel_ = std::move(h);
    }

    /**
     * @brief Set pubcomp handler
     * @param h handler
     */
    void set_v5_pubcomp_handler(v5_pubcomp_handler h = v5_pubcomp_handler()) {
        h_v5_pubcomp_ = std::move(h);
    }

    /**
     * @brief Set subscribe handler
     * @param h handler
     */
    void set_v5_subscribe_handler(v5_subscribe_handler h = v5_subscribe_handler()) {
        h_v5_subscribe_ = std::move(h);
    }

    /**
     * @brief Set suback handler
     * @param h handler
     */
    void set_v5_suback_handler(v5_suback_handler h = v5_suback_handler()) {
        h_v5_suback_ = std::move(h);
    }

    /**
     * @brief Set unsubscribe handler
     * @param h handler
     */
    void set_v5_unsubscribe_handler(v5_unsubscribe_handler h = v5_unsubscribe_handler()) {
        h_v5_unsubscribe_ = std::move(h);
    }

    /**
     * @brief Set unsuback handler
     * @param h handler
     */
    void set_v5_unsuback_handler(v5_unsuback_handler h = v5_unsuback_handler()) {
        h_v5_unsuback_ = std::move(h);
    }

    /**
     * @brief Set disconnect handler
     * @param h handler
     */
    void set_v5_disconnect_handler(v5_disconnect_handler h = v5_disconnect_handler()) {
        h_v5_disconnect_ = std::move(h);
    }

    /**
     * @brief Get connect handler
     * @return handler
     */
    v5_connect_handler get_v5_connect_handler() const {
        return h_v5_connect_;
    }

    /**
     * @brief Get connack handler
     * @return handler
     */
    v5_connack_handler get_v5_connack_handler() const {
        return h_v5_connack_;
    }

    /**
     * @brief Set publish handler
     * @return handler
     */
    v5_publish_handler get_v5_publish_handler() const {
        return h_v5_publish_;
    }

    /**
     * @brief Get puback handler
     * @return handler
     */
    v5_puback_handler get_v5_puback_handler() const {
        return h_v5_puback_;
    }

    /**
     * @brief Get pubrec handler
     * @return handler
     */
    v5_pubrec_handler get_v5_pubrec_handler() const {
        return h_v5_pubrec_;
    }

    /**
     * @brief Get pubrel handler
     * @return handler
     */
    v5_pubrel_handler get_v5_pubrel_handler() const {
        return h_v5_pubrel_;
    }

    /**
     * @brief Get pubcomp handler
     * @return handler
     */
    v5_pubcomp_handler get_v5_pubcomp_handler() const {
        return h_v5_pubcomp_;
    }

    /**
     * @brief Get subscribe handler
     * @return handler
     */
    v5_subscribe_handler get_v5_subscribe_handler() const {
        return h_v5_subscribe_;
    }

    /**
     * @brief Get suback handler
     * @return handler
     */
    v5_suback_handler get_v5_suback_handler() const {
        return h_v5_suback_;
    }

    /**
     * @brief Get unsubscribe handler
     * @return handler
     */
    v5_unsubscribe_handler get_v5_unsubscribe_handler() const {
        return h_v5_unsubscribe_;
    }

    /**
     * @brief Get unsuback handler
     * @return handler
     */
    v5_unsuback_handler get_v5_unsuback_handler() const {
        return h_v5_unsuback_;
    }

    /**
     * @brief Get disconnect handler
     * @return handler
     */
    v5_disconnect_handler get_v5_disconnect_handler() const {
        return h_v5_disconnect_;
    }


    // Original handlers

    /**
     * @brief Set close handler
     * @param h handler
     */
    void set_close_handler(close_handler h = close_handler()) {
        h_close_ = std::move(h);
    }

    /**
     * @brief Set error handler
     * @param h handler
     */
    void set_error_handler(error_handler h = error_handler()) {
        h_error_ = std::move(h);
    }

    /**
     * @brief Set pubcomp handler
     * @param h handler
     */
    void set_pub_res_sent_handler(pub_res_sent_handler h = pub_res_sent_handler()) {
        h_pub_res_sent_ = std::move(h);
    }

    /**
     * @brief Set serialize handlers
     * @param h_publish serialize handler for publish message
     * @param h_pubrel serialize handler for pubrel message
     * @param h_remove remove handler for serialized message
     */
    void set_serialize_handlers(
        serialize_publish_message_handler h_publish,
        serialize_pubrel_message_handler h_pubrel,
        serialize_remove_handler h_remove) {
        h_serialize_publish_ = std::move(h_publish);
        h_serialize_pubrel_ = std::move(h_pubrel);
        h_serialize_remove_ = std::move(h_remove);
    }

    /**
     * @brief Set serialize handlers
     * @param h_publish serialize handler for publish message
     * @param h_pubrel serialize handler for pubrel message
     * @param h_remove remove handler for serialized message
     */
    void set_v5_serialize_handlers(
        serialize_v5_publish_message_handler h_publish,
        serialize_v5_pubrel_message_handler h_pubrel,
        serialize_remove_handler h_remove) {
        h_serialize_v5_publish_ = std::move(h_publish);
        h_serialize_v5_pubrel_ = std::move(h_pubrel);
        h_serialize_remove_ = std::move(h_remove);
    }

    /**
     * @brief Set serialize handlers
     * @param h_publish serialize handler for publish message
     * @param h_pubrel serialize handler for pubrel message
     * @param h_remove remove handler for serialized message
     */
    void set_serialize_handlers(
        serialize_publish_handler h_publish,
        serialize_pubrel_handler h_pubrel,
        serialize_remove_handler h_remove) {
        h_serialize_publish_ =
            [MQTT_CAPTURE_MOVE(h_publish)]
            (basic_publish_message<PacketIdBytes> msg) {
                if (h_publish) {
                    auto buf = continuous_buffer<PacketIdBytes>(msg);
                    h_publish(msg.packet_id(), buf.data(), buf.size());
                }
            };
        h_serialize_pubrel_ =
            [MQTT_CAPTURE_MOVE(h_pubrel)]
            (basic_pubrel_message<PacketIdBytes> msg) {
                if (h_pubrel) {
                    auto buf = continuous_buffer<PacketIdBytes>(msg);
                    h_pubrel(msg.packet_id(), buf.data(), buf.size());
                }
            };
        h_serialize_remove_ = std::move(h_remove);
    }

    /**
     * @brief Set serialize handlers
     * @param h_publish serialize handler for publish message
     * @param h_pubrel serialize handler for pubrel message
     * @param h_remove remove handler for serialized message
     */
    void set_v5_serialize_handlers(
        serialize_publish_handler h_publish,
        serialize_pubrel_handler h_pubrel,
        serialize_remove_handler h_remove) {
        h_serialize_v5_publish_ =
            [MQTT_CAPTURE_MOVE(h_publish)]
            (v5::basic_publish_message<PacketIdBytes> msg) {
                if (h_publish) {
                    auto buf = continuous_buffer<PacketIdBytes>(msg);
                    h_publish(msg.packet_id(), buf.data(), buf.size());
                }
            };
        h_serialize_v5_pubrel_ =
            [MQTT_CAPTURE_MOVE(h_pubrel)]
            (v5::basic_pubrel_message<PacketIdBytes> msg) {
                if (h_pubrel) {
                    auto buf = continuous_buffer<PacketIdBytes>(msg);
                    h_pubrel(msg.packet_id(), buf.data(), buf.size());
                }
            };
        h_serialize_remove_ = std::move(h_remove);
    }

    /**
     * @brief Clear serialize handlers
     */
    void set_serialize_handlers() {
        h_serialize_publish_ = serialize_publish_message_handler();
        h_serialize_pubrel_ = serialize_pubrel_message_handler();
        h_serialize_v5_publish_ = serialize_v5_publish_message_handler();
        h_serialize_v5_pubrel_ = serialize_v5_pubrel_message_handler();
        h_serialize_remove_ = serialize_remove_handler();
    }

    /**
     * @brief Set pre-send handler
     * @param h handler
     */
    void set_pre_send_handler(pre_send_handler h = pre_send_handler()) {
        h_pre_send_ = std::move(h);
    }

    /**
     * @brief Set check length handler
     * @param h handler
     */
    void set_is_valid_length_handler(is_valid_length_handler h = is_valid_length_handler()) {
        h_is_valid_length_ = std::move(h);
    }


    /**
     * @brief Get close handler
     * @return handler
     */
    close_handler get_close_handler() const {
        return h_close_;
    }

    /**
     * @brief Get error handler
     * @return handler
     */
    error_handler get_error_handler() const {
        return h_error_;
    }


    /**
     * @brief Get pubcomp handler
     * @return handler
     */
    pub_res_sent_handler get_pub_res_sent_handler() const {
        return h_pub_res_sent_;
    }

    /**
     * @brief Get serialize publish handler
     * @return handler
     */
    serialize_publish_message_handler get_serialize_publish_message_handler() const {
        return h_serialize_publish_;
    }

    /**
     * @brief Get serialize pubrel handler
     * @return handler
     */
    serialize_pubrel_message_handler get_serialize_pubrel_message_handler() const {
        return h_serialize_pubrel_;
    }

    /**
     * @brief Get serialize publish handler
     * @return handler
     */
    serialize_v5_publish_message_handler get_serialize_v5_publish_message_handler() const {
        return h_serialize_v5_publish_;
    }

    /**
     * @brief Get serialize pubrel handler
     * @return handler
     */
    serialize_v5_pubrel_message_handler get_serialize_v5_pubrel_message_handler() const {
        return h_serialize_v5_pubrel_;
    }

    /**
     * @brief Get serialize remove handler
     * @return handler
     */
    serialize_remove_handler get_serialize_remove_handler() const {
        return h_serialize_remove_;
    }

    /**
     * @brief Get pre-send handler
     * @return handler
     */
    pre_send_handler get_pre_send_handler() const {
        return h_pre_send_;
    }

    /**
     * @brief Get check length handler
     * @return handler
     */
    is_valid_length_handler get_is_valid_length_handler() const {
        return h_is_valid_length_;
    }


    /**
     * @brief start session with a connected endpoint.
     * @param func finish handler that is called when the session is finished
     *
     */
    void start_session(async_handler_t const& func = async_handler_t()) {
        async_read_control_packet_type(func);
    }

    // Blocking APIs

    /**
     * @brief Publish QoS0
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     */
    void publish_at_most_once(
        std::string const& topic_name,
        std::string const& contents,
        bool retain = false,
        boost::optional<std::vector<v5::property_variant>> props = boost::none
    ) {
        acquired_publish(0, topic_name, contents, qos::at_most_once, retain, std::move(props));
    }

    /**
     * @brief Publish QoS0
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     */
    void publish_at_most_once(
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        bool retain = false) {
        acquired_publish(0, topic_name, contents, [] {}, qos::at_most_once, retain);
    }

    /**
     * @brief Publish QoS1
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @return packet_id
     * packet_id is automatically generated.
     */
    packet_id_t publish_at_least_once(
        std::string const& topic_name,
        std::string const& contents,
        bool retain = false) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_publish_at_least_once(packet_id, topic_name, contents, retain);
        return packet_id;
    }

    /**
     * @brief Publish QoS1
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @return packet_id
     * packet_id is automatically generated.
     */
    packet_id_t publish_at_least_once(
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        bool retain = false) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_publish_at_least_once(packet_id, topic_name, contents, life_keeper, retain);
        return packet_id;
    }

    /**
     * @brief Publish QoS2
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @return packet_id
     * packet_id is automatically generated.
     */
    packet_id_t publish_exactly_once(
        std::string const& topic_name,
        std::string const& contents,
        bool retain = false) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_publish_exactly_once(packet_id, topic_name, contents, retain);
        return packet_id;
    }

    /**
     * @brief Publish QoS2
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @return packet_id
     * packet_id is automatically generated.
     */
    packet_id_t publish_exactly_once(
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        bool retain = false) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_publish_exactly_once(packet_id, topic_name, contents, life_keeper, retain);
        return packet_id;
    }

    /**
     * @brief Publish
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @return packet_id. If qos is set to at_most_once, return 0.
     * packet_id is automatically generated.
     */
    packet_id_t publish(
        std::string const& topic_name,
        std::string const& contents,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        packet_id_t packet_id = qos == qos::at_most_once ? 0 : acquire_unique_packet_id();
        acquired_publish(packet_id, topic_name, contents, qos, retain);
        return packet_id;
    }

    /**
     * @brief Publish
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @return packet_id. If qos is set to at_most_once, return 0.
     * packet_id is automatically generated.
     */
    packet_id_t publish(
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        packet_id_t packet_id = qos == qos::at_most_once ? 0 : acquire_unique_packet_id();
        acquired_publish(packet_id, topic_name, contents, life_keeper, qos, retain);
        return packet_id;
    }

    /**
     * @brief Subscribe
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param args
     *        args should be zero or more pairs of topic_name and qos.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    template <typename... Args>
    packet_id_t subscribe(
        std::string const& topic_name,
        std::uint8_t qos, Args... args) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_subscribe(packet_id, topic_name, qos, std::forward<Args>(args)...);
        return packet_id;
    }

    /**
     * @brief Subscribe
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param args
     *        args should be zero or more pairs of topic_name and qos.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    template <typename... Args>
    packet_id_t subscribe(
        as::const_buffer const& topic_name,
        std::uint8_t qos, Args... args) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_subscribe(packet_id, topic_name, qos, std::forward<Args>(args)...);
        return packet_id;
    }

    /**
     * @brief Subscribe
     * @param params a vector of the topic_filter and qos pair.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    packet_id_t subscribe(
        std::vector<std::tuple<std::string, std::uint8_t>> const& params
    ) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_subscribe(packet_id, params);
        return packet_id;
    }

    /**
     * @brief Subscribe
     * @param params a vector of the topic_filter and qos pair.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    packet_id_t subscribe(
        std::vector<std::tuple<as::const_buffer, std::uint8_t>> const& params
    ) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_subscribe(packet_id, params);
        return packet_id;
    }

    /**
     * @brief Unsubscribe
     * @param topic_name
     *        A topic name to subscribe
     * @param args
     *        args should be zero or more topics
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    template <typename... Args>
    packet_id_t unsubscribe(
        std::string const& topic_name,
        Args... args) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_unsubscribe(packet_id, topic_name, std::forward<Args>(args)...);
        return packet_id;
    }

    /**
     * @brief Unsubscribe
     * @param topic_name
     *        A topic name to subscribe
     * @param args
     *        args should be zero or more topics
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    template <typename... Args>
    packet_id_t unsubscribe(
        as::const_buffer const& topic_name,
        Args... args) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_unsubscribe(packet_id, topic_name, std::forward<Args>(args)...);
        return packet_id;
    }

    /**
     * @brief Unsubscribe
     * @param params a collection of topic_filter.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    packet_id_t unsubscribe(
        std::vector<std::string> const& params
    ) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_unsubscribe(packet_id, params);
        return packet_id;
    }

    /**
     * @brief Unsubscribe
     * @param params a collection of topic_filter.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    packet_id_t unsubscribe(
        std::vector<as::const_buffer> const& params
    ) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_unsubscribe(packet_id, params);
        return packet_id;
    }

    /**
     * @brief Disconnect
     * Send a disconnect packet to the connected broker. It is a clean disconnecting sequence.
     * The broker disconnects the endpoint after receives the disconnect packet.<BR>
     * When the endpoint disconnects using disconnect(), a will won't send.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718090<BR>
     */
    void disconnect() {
        if (connected_ && mqtt_connected_) {
            disconnect_requested_ = true;
            send_disconnect();
        }
    }

    /**
     * @brief Disconnect by endpoint
     * Force disconnect. It is not a clean disconnect sequence.<BR>
     * When the endpoint disconnects using force_disconnect(), a will will send.<BR>
     */
    void force_disconnect() {
        connected_ = false;
        mqtt_connected_ = false;
        shutdown_from_client(*socket_);
    }


    // packet_id manual setting version

    /**
     * @brief Publish QoS1 with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         contents doesn't publish, otherwise return true and contents publish.
     */
    bool publish_at_least_once(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::string const& contents,
        bool retain = false) {
        if (register_packet_id(packet_id)) {
            acquired_publish_at_least_once(packet_id, topic_name, contents, retain);
            return true;
        }
        return false;
    }

    /**
     * @brief Publish QoS1 with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         contents doesn't publish, otherwise return true and contents publish.
     */
    bool publish_at_least_once(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        bool retain = false) {
        if (register_packet_id(packet_id)) {
            acquired_publish_at_least_once(packet_id, topic_name, contents, life_keeper, retain);
            return true;
        }
        return false;
    }

    /**
     * @brief Publish QoS2 with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         contents doesn't publish, otherwise return true and contents publish.
     */
    bool publish_exactly_once(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::string const& contents,
        bool retain = false) {
        if (register_packet_id(packet_id)) {
            acquired_publish_exactly_once(packet_id, topic_name, contents, retain);
            return true;
        }
        return false;
    }

    /**
     * @brief Publish QoS2 with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         contents doesn't publish, otherwise return true and contents publish.
     */
    bool publish_exactly_once(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        bool retain = false) {
        if (register_packet_id(packet_id)) {
            acquired_publish_exactly_once(packet_id, topic_name, contents, life_keeper, retain);
            return true;
        }
        return false;
    }

    /**
     * @brief Publish with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         contents don't publish, otherwise return true and contents publish.
     */
    bool publish(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::string const& contents,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        if (register_packet_id(packet_id)) {
            acquired_publish(packet_id, topic_name, contents, qos, retain);
            return true;
        }
        return false;
    }

    /**
     * @brief Publish with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         contents don't publish, otherwise return true and contents publish.
     */
    bool publish(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        if (register_packet_id(packet_id)) {
            acquired_publish(packet_id, topic_name, contents, life_keeper, qos, retain);
            return true;
        }
        return false;
    }

    /**
     * @brief Publish as dup with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         contents don't publish, otherwise return true and contents publish.
     */
    bool publish_dup(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::string const& contents,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        if (register_packet_id(packet_id)) {
            acquired_publish_dup(packet_id, topic_name, contents, qos, retain);
            return true;
        }
        return false;
    }

    /**
     * @brief Publish as dup with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         contents don't publish, otherwise return true and contents publish.
     */
    bool publish_dup(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        if (register_packet_id(packet_id)) {
            acquired_publish_dup(packet_id, topic_name, contents, life_keeper, qos, retain);
            return true;
        }
        return false;
    }

    /**
     * @brief Subscribe with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param args
     *        args should be zero or more pairs of topic_name and qos.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't subscribe, otherwise return true and subscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066<BR>
     */
    template <typename... Args>
    bool subscribe(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::uint8_t qos, Args... args) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        if (register_packet_id(packet_id)) {
            acquired_subscribe(packet_id, topic_name,  qos, std::forward<Args>(args)...);
            return true;
        }
        return false;
    }

    /**
     * @brief Subscribe with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param args
     *        args should be zero or more pairs of topic_name and qos.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't subscribe, otherwise return true and subscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066<BR>
     */
    template <typename... Args>
    bool subscribe(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        std::uint8_t qos, Args... args) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        if (register_packet_id(packet_id)) {
            acquired_subscribe(packet_id, topic_name,  qos, std::forward<Args>(args)...);
            return true;
        }
        return false;
    }

    /**
     * @brief Subscribe with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param params a vector of the topic_filter and qos pair.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't subscribe, otherwise return true and subscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066<BR>
     */
    bool subscribe(
        packet_id_t packet_id,
        std::vector<std::tuple<std::string, std::uint8_t>> const& params
    ) {
        if (register_packet_id(packet_id)) {
            acquired_subscribe(packet_id, params);
            return true;
        }
        return false;
    }

    /**
     * @brief Subscribe with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param params a vector of the topic_filter and qos pair.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't subscribe, otherwise return true and subscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066<BR>
     */
    bool subscribe(
        packet_id_t packet_id,
        std::vector<std::tuple<as::const_buffer, std::uint8_t>> const& params
    ) {
        if (register_packet_id(packet_id)) {
            acquired_subscribe(packet_id, params);
            return true;
        }
        return false;
    }

    /**
     * @brief Unsubscribe with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to subscribe
     * @param args
     *        args should be zero or more topics
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't unsubscribe, otherwise return true and unsubscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    template <typename... Args>
    bool unsubscribe(
        packet_id_t packet_id,
        std::string const& topic_name,
        Args... args) {
        if (register_packet_id(packet_id)) {
            acquired_unsubscribe(packet_id, topic_name, std::forward<Args>(args)...);
            return true;
        }
        return false;
    }

    /**
     * @brief Unsubscribe with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to subscribe
     * @param args
     *        args should be zero or more topics
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't unsubscribe, otherwise return true and unsubscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    template <typename... Args>
    bool unsubscribe(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        Args... args) {
        if (register_packet_id(packet_id)) {
            acquired_unsubscribe(packet_id, topic_name, std::forward<Args>(args)...);
            return true;
        }
        return false;
    }

    /**
     * @brief Unsubscribe with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param params a collection of topic_filter
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't unsubscribe, otherwise return true and unsubscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    bool unsubscribe(
        packet_id_t packet_id,
        std::vector<std::string> const& params
    ) {
        if (register_packet_id(packet_id)) {
            acquired_unsubscribe(packet_id, params);
            return true;
        }
        return false;
    }

    /**
     * @brief Unsubscribe with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param params a collection of topic_filter
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't unsubscribe, otherwise return true and unsubscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    bool unsubscribe(
        packet_id_t packet_id,
        std::vector<as::const_buffer> const& params
    ) {
        if (register_packet_id(packet_id)) {
            acquired_unsubscribe(packet_id, params);
            return true;
        }
        return false;
    }

    // packet_id has already been acquired version

    /**
     * @brief Publish QoS1 with already acquired packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     */
    void acquired_publish_at_least_once(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::string const& contents,
        bool retain = false) {

        auto sp_topic_name = std::make_shared<std::string>(topic_name);
        auto sp_contents = std::make_shared<std::string>(contents);

        send_publish(
            as::buffer(*sp_topic_name),
            qos::at_least_once,
            retain,
            false,
            packet_id,
            boost::none,
            as::buffer(*sp_contents),
            [sp_topic_name, sp_contents] {}
        );
    }

    /**
     * @brief Publish QoS1 with already acquired packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     */
    void acquired_publish_at_least_once(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        bool retain = false) {

        send_publish(
            topic_name,
            qos::at_least_once,
            retain,
            false,
            packet_id,
            boost::none,
            contents,
            life_keeper
        );
    }

    /**
     * @brief Publish QoS2 with already acquired packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     */
    void acquired_publish_exactly_once(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::string const& contents,
        bool retain = false) {

        auto sp_topic_name = std::make_shared<std::string>(topic_name);
        auto sp_contents = std::make_shared<std::string>(contents);

        send_publish(
            as::buffer(*sp_topic_name),
            qos::exactly_once,
            retain,
            false,
            packet_id,
            boost::none,
            as::buffer(*sp_contents),
            [sp_topic_name, sp_contents] {}
        );
    }

    /**
     * @brief Publish QoS2 with already acquired packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     */
    void acquired_publish_exactly_once(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        bool retain = false) {

        send_publish(
            topic_name,
            qos::exactly_once,
            retain,
            false,
            packet_id,
            boost::none,
            contents,
            life_keeper
        );
    }

    /**
     * @brief Publish with already acquired packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     *        If qos == qos::at_most_once, packet_id must be 0. But not checked in release mode due to performance.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     */
    void acquired_publish(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::string const& contents,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false,
        boost::optional<std::vector<v5::property_variant>> props = boost::none
    ) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        BOOST_ASSERT((qos == qos::at_most_once && packet_id == 0) || (qos != qos::at_most_once && packet_id != 0));
        auto sp_topic_name = std::make_shared<std::string>(topic_name);
        auto sp_contents = std::make_shared<std::string>(contents);

        send_publish(
            as::buffer(*sp_topic_name),
            qos,
            retain,
            false,
            packet_id,
            std::move(props),
            as::buffer(*sp_contents),
            [sp_topic_name, sp_contents] {}
        );
    }

    /**
     * @brief Publish with already acquired packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     *        If qos == qos::at_most_once, packet_id must be 0. But not checked in release mode due to performance.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     */
    void acquired_publish(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false,
        boost::optional<std::vector<v5::property_variant>> props = boost::none
    ) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        BOOST_ASSERT((qos == qos::at_most_once && packet_id == 0) || (qos != qos::at_most_once && packet_id != 0));

        send_publish(
            topic_name,
            qos,
            retain,
            false,
            packet_id,
            std::move(props),
            contents,
            life_keeper
        );
    }

    /**
     * @brief Publish as dup with already acquired packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     *        If qos == qos::at_most_once, packet_id must be 0. But not checked in release mode due to performance.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     */
    void acquired_publish_dup(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::string const& contents,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false,
        boost::optional<std::vector<v5::property_variant>> props = boost::none
    ) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        BOOST_ASSERT((qos == qos::at_most_once && packet_id == 0) || (qos != qos::at_most_once && packet_id != 0));

        auto sp_topic_name = std::make_shared<std::string>(topic_name);
        auto sp_contents = std::make_shared<std::string>(contents);

        send_publish(
            as::buffer(*sp_topic_name),
            qos,
            retain,
            true,
            packet_id,
            std::move(props),
            as::buffer(*sp_contents),
            [sp_topic_name, sp_contents] {}
        );
    }

    /**
     * @brief Publish as dup with already acquired packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     *        If qos == qos::at_most_once, packet_id must be 0. But not checked in release mode due to performance.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     */
    void acquired_publish_dup(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false,
        boost::optional<std::vector<v5::property_variant>> props = boost::none
    ) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        BOOST_ASSERT((qos == qos::at_most_once && packet_id == 0) || (qos != qos::at_most_once && packet_id != 0));

        send_publish(
            topic_name,
            qos,
            retain,
            true,
            packet_id,
            std::move(props),
            contents,
            life_keeper
        );
    }

    /**
     * @brief Subscribe with already acquired packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param args
     *        args should be zero or more pairs of topic_name and qos.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066<BR>
     */
    template <typename... Args>
    void acquired_subscribe(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::uint8_t qos, Args... args) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        std::vector<std::tuple<as::const_buffer, std::uint8_t>> params;
        params.reserve((sizeof...(args) + 2) / 2);
        send_subscribe(params, packet_id, as::buffer(topic_name), qos, args...);
    }

    /**
     * @brief Subscribe with already acquired packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param args
     *        args should be zero or more pairs of topic_name and qos.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066<BR>
     */
    template <typename... Args>
    void acquired_subscribe(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        std::uint8_t qos, Args... args) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        std::vector<std::tuple<as::const_buffer, std::uint8_t>> params;
        params.reserve((sizeof...(args) + 2) / 2);
        send_subscribe(params, packet_id, topic_name, qos, args...);
    }

    /**
     * @brief Subscribe with already acquired packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param params a vector of the topic_filter and qos pair.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066<BR>
     */
    void acquired_subscribe(
        packet_id_t packet_id,
        std::vector<std::tuple<std::string, std::uint8_t>> const& params
    ) {
        std::vector<std::tuple<as::const_buffer, std::uint8_t>> cb_params;
        cb_params.reserve(params.size());
        for (auto const& e : params) {
            cb_params.emplace_back(as::buffer(std::get<0>(e)), std::get<1>(e));
        }
        send_subscribe(cb_params, packet_id);
    }

    /**
     * @brief Subscribe with already acquired packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param params a vector of the topic_filter and qos pair.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066<BR>
     */
    void acquired_subscribe(
        packet_id_t packet_id,
        std::vector<std::tuple<as::const_buffer, std::uint8_t>> const& params
    ) {
        send_subscribe(params, packet_id);
    }

    /**
     * @brief Unsubscribe with already acquired packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name
     *        A topic name to subscribe
     * @param args
     *        args should be zero or more topics
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    template <typename... Args>
    void acquired_unsubscribe(
        packet_id_t packet_id,
        std::string const& topic_name,
        Args... args) {
        std::vector<as::const_buffer> params;
        params.reserve(sizeof...(args) + 1);
        send_unsubscribe(params, packet_id, as::buffer(topic_name), args...);
    }

    /**
     * @brief Unsubscribe with already acquired packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name
     *        A topic name to subscribe
     * @param args
     *        args should be zero or more topics
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    template <typename... Args>
    void acquired_unsubscribe(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        Args... args) {

        std::vector<as::const_buffer> params;
        params.reserve(sizeof...(args) + 1);
        send_unsubscribe(params, packet_id, topic_name, args...);
    }

    /**
     * @brief Unsubscribe with already acquired packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param params a collection of topic_filter
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    void acquired_unsubscribe(
        packet_id_t packet_id,
        std::vector<std::string> const& params
    ) {
        std::vector<as::const_buffer> cb_params;
        cb_params.reserve(params.size());

        for (auto const& e : params) {
            cb_params.emplace_back(as::buffer(e));
        }
        send_unsubscribe(cb_params, packet_id);
    }

    /**
     * @brief Unsubscribe with already acquired packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param params a collection of topic_filter
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    void acquired_unsubscribe(
        packet_id_t packet_id,
        std::vector<as::const_buffer> const& params
    ) {
        send_unsubscribe(params, packet_id);
    }

    /**
     * @brief Send pingreq packet.
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    void pingreq() {
        if (connected_ && mqtt_connected_) send_pingreq();
    }

    /**
     * @brief Send pingresp packet. This function is for broker.
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718086
     */
    void pingresp() {
        send_pingresp();
    }

    /**
     * @brief Send connect packet.
     * @param keep_alive_sec See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349238
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718028
     */
    void connect(std::uint16_t keep_alive_sec) {
        connect_requested_ = true;
        send_connect(keep_alive_sec);
    }

    /**
     * @brief Send connack packet. This function is for broker.
     * @param session_present See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349255
     * @param return_code See connect_return_code.hpp and http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349256
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718033
     */
    void connack(bool session_present, std::uint8_t return_code) {
        send_connack(session_present, return_code);
    }

    /**
     * @brief Send puback packet.
     * @param packet_id packet id corresponding to publish
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718043
     */
    void puback(packet_id_t packet_id) {
        send_puback(packet_id);
    }

    /**
     * @brief Send pubrec packet.
     * @param packet_id packet id corresponding to publish
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718048
     */
    void pubrec(packet_id_t packet_id) {
        send_pubrec(packet_id);
    }

    /**
     * @brief Send pubrel packet.
     * @param packet_id packet id corresponding to publish
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718053
     */
    void pubrel(packet_id_t packet_id) {
        send_pubrel(packet_id);
    }

    /**
     * @brief Send pubcomp packet.
     * @param packet_id packet id corresponding to publish
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718058
     */
    void pubcomp(packet_id_t packet_id) {
        send_pubcomp(packet_id);
    }

    /**
     * @brief Send suback packet. This function is for broker.
     * @param packet_id packet id corresponding to subscribe
     * @param qos adjusted qos
     * @param args additional qos
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718068
     */
    template <typename... Args>
    void suback(
        packet_id_t packet_id,
        std::uint8_t qos, Args&&... args) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        std::vector<std::uint8_t> params;
        send_suback(params, packet_id, qos, std::forward<Args>(args)...);
    }

    /**
     * @brief Send suback packet. This function is for broker.
     * @param packet_id packet id corresponding to subscribe
     * @param qoss a collection of adjusted qos
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718068
     */
    void suback(
        packet_id_t packet_id,
        std::vector<std::uint8_t> const& qoss) {
        send_suback(qoss, packet_id);
    }

    /**
     * @brief Send unsuback packet. This function is for broker.
     * @param packet_id packet id corresponding to unsubscribe
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718077
     */
    void unsuback(
        packet_id_t packet_id) {
        send_unsuback(packet_id);
    }


    /**
     * @brief Publish QoS0
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     */
    void async_publish_at_most_once(
        std::string const& topic_name,
        std::string const& contents,
        bool retain = false,
        async_handler_t const& func = async_handler_t()) {
        acquired_async_publish(0, topic_name, contents, qos::at_most_once, retain, func);
    }

    /**
     * @brief Publish QoS0
     *        topic_name and contents are reference type. So caller need to keep the lifetime of them
     *        until func is called.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     */
    void async_publish_at_most_once(
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        bool retain = false,
        async_handler_t const& func = async_handler_t()) {
        acquired_async_publish(0, topic_name, contents, [] {}, qos::at_most_once, retain, func);
    }

    /**
     * @brief Publish QoS1
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     * @return packet_id
     * packet_id is automatically generated.
     */
    packet_id_t async_publish_at_least_once(
        std::string const& topic_name,
        std::string const& contents,
        bool retain = false,
        async_handler_t const& func = async_handler_t()) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_async_publish_at_least_once(packet_id, topic_name, contents, retain, func);
        return packet_id;
    }

    /**
     * @brief Publish QoS1
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     * @return packet_id
     * packet_id is automatically generated.
     */
    packet_id_t async_publish_at_least_once(
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        bool retain = false,
        async_handler_t const& func = async_handler_t()) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_async_publish_at_least_once(packet_id, topic_name, contents, life_keeper, retain, func);
        return packet_id;
    }

    /**
     * @brief Publish QoS2
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     * @return packet_id
     * packet_id is automatically generated.
     */
    packet_id_t async_publish_exactly_once(
        std::string const& topic_name,
        std::string const& contents,
        bool retain = false,
        async_handler_t const& func = async_handler_t()) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_async_publish_exactly_once(packet_id, topic_name, contents, retain, func);
        return packet_id;
    }

    /**
     * @brief Publish QoS2
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     * @return packet_id
     * packet_id is automatically generated.
     */
    packet_id_t async_publish_exactly_once(
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        bool retain = false,
        async_handler_t const& func = async_handler_t()) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_async_publish_exactly_once(packet_id, topic_name, contents, life_keeper, retain, func);
        return packet_id;
    }

    /**
     * @brief Publish
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     * @return packet_id. If qos is set to at_most_once, return 0.
     * packet_id is automatically generated.
     */
    packet_id_t async_publish(
        std::string const& topic_name,
        std::string const& contents,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false,
        async_handler_t const& func = async_handler_t()) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        packet_id_t packet_id = qos == qos::at_most_once ? 0 : acquire_unique_packet_id();
        acquired_async_publish(packet_id, topic_name, contents, qos, retain, func);
        return packet_id;
    }

    /**
     * @brief Publish
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     * @return packet_id. If qos is set to at_most_once, return 0.
     * packet_id is automatically generated.
     */
    packet_id_t async_publish(
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false,
        async_handler_t const& func = async_handler_t()) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        packet_id_t packet_id = qos == qos::at_most_once ? 0 : acquire_unique_packet_id();
        acquired_async_publish(packet_id, topic_name, contents, life_keeper, qos, retain, func);
        return packet_id;
    }

    /**
     * @brief Subscribe
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param args
     *        args should be some pairs of topic_name and qos to subscribe, <BR>
     *        and the last one is a callback function that is called when async operation will finish.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    template <typename... Args>
    typename std::enable_if<
        sizeof...(Args) != 0,
        packet_id_t
    >::type
    async_subscribe(
        std::string const& topic_name,
        std::uint8_t qos, Args&&... args) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_async_subscribe(packet_id, topic_name, qos, std::forward<Args>(args)...);
        return packet_id;
    }

    /**
     * @brief Subscribe
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param args
     *        args should be some pairs of topic_name and qos to subscribe, <BR>
     *        and the last one is a callback function that is called when async operation will finish.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    template <typename... Args>
    typename std::enable_if<
        sizeof...(Args) != 0,
        packet_id_t
    >::type
    async_subscribe(
        as::const_buffer const& topic_name,
        std::uint8_t qos, Args&&... args) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_async_subscribe(packet_id, topic_name, qos, std::forward<Args>(args)...);
        return packet_id;
    }

    /**
     * @brief Subscribe
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param func A callback function that is called when async operation will finish.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    packet_id_t async_subscribe(
        std::string const& topic_name,
        std::uint8_t qos,
        async_handler_t const& func = async_handler_t()) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_async_subscribe(packet_id, topic_name, qos, func);
        return packet_id;
    }

    /**
     * @brief Subscribe
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param func A callback function that is called when async operation will finish.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    packet_id_t async_subscribe(
        as::const_buffer const& topic_name,
        std::uint8_t qos,
        async_handler_t const& func = async_handler_t()) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_async_subscribe(packet_id, topic_name, qos, func);
        return packet_id;
    }

    /**
     * @brief Subscribe
     * @param params A collection of the pair of topic_name and qos to subscribe.
     * @param func A callback function that is called when async operation will finish.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    packet_id_t async_subscribe(
        std::vector<std::tuple<std::string, std::uint8_t>> const& params,
        async_handler_t const& func = async_handler_t()) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_async_subscribe(packet_id, params, func);
        return packet_id;
    }

    /**
     * @brief Subscribe
     * @param params A collection of the pair of topic_name and qos to subscribe.
     * @param func A callback function that is called when async operation will finish.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    packet_id_t async_subscribe(
        std::vector<std::tuple<as::const_buffer, std::uint8_t>> const& params,
        async_handler_t const& func = async_handler_t()) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_async_subscribe(packet_id, params, func);
        return packet_id;
    }

    /**
     * @brief Unsubscribe
     * @param topic_name
     *        A topic name to unsubscribe
     * @param args
     *        args should be some topic_names to unsubscribe, <BR>
     *        and the last one is a callback function that is called when async operation will finish.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    template <typename... Args>
    typename std::enable_if<
        sizeof...(Args) != 0,
        packet_id_t
    >::type
    async_unsubscribe(
        std::string const& topic_name,
        Args&&... args) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_async_unsubscribe(packet_id, topic_name, std::forward<Args>(args)...);
        return packet_id;
    }

    /**
     * @brief Unsubscribe
     * @param topic_name
     *        A topic name to unsubscribe
     * @param args
     *        args should be some topic_names to unsubscribe, <BR>
     *        and the last one is a callback function that is called when async operation will finish.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    template <typename... Args>
    typename std::enable_if<
        sizeof...(Args) != 0,
        packet_id_t
    >::type
    async_unsubscribe(
        as::const_buffer const& topic_name,
        Args&&... args) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_async_unsubscribe(packet_id, topic_name, std::forward<Args>(args)...);
        return packet_id;
    }

    /**
     * @brief Unsubscribe
     * @param topic_name
     *        A topic name to unsubscribe
     * @param func A callback function that is called when async operation will finish.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    packet_id_t async_unsubscribe(
        std::string const& topic_name,
        async_handler_t const& func = async_handler_t()) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_async_unsubscribe(packet_id, topic_name, func);
        return packet_id;
    }

    /**
     * @brief Unsubscribe
     * @param topic_name
     *        A topic name to unsubscribe
     * @param func A callback function that is called when async operation will finish.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    packet_id_t async_unsubscribe(
        as::const_buffer const& topic_name,
        async_handler_t const& func = async_handler_t()) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_async_unsubscribe(packet_id, topic_name, func);
        return packet_id;
    }

    /**
     * @brief Unsubscribe
     * @param params
     *        A collection of the topic name to unsubscribe
     * @param func A callback function that is called when async operation will finish.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    packet_id_t async_unsubscribe(
        std::vector<std::string> const& params,
        async_handler_t const& func = async_handler_t()) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_async_unsubscribe(packet_id, params, func);
        return packet_id;
    }

    /**
     * @brief Unsubscribe
     * @param params
     *        A collection of the topic name to unsubscribe
     * @param func A callback function that is called when async operation will finish.
     * @return packet_id.
     * packet_id is automatically generated.<BR>
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    packet_id_t async_unsubscribe(
        std::vector<as::const_buffer> const& params,
        async_handler_t const& func = async_handler_t()) {
        packet_id_t packet_id = acquire_unique_packet_id();
        acquired_async_unsubscribe(packet_id, params, func);
        return packet_id;
    }

    /**
     * @brief Disconnect
     * @param func A callback function that is called when async operation will finish.
     * Send a disconnect packet to the connected broker. It is a clean disconnecting sequence.
     * The broker disconnects the endpoint after receives the disconnect packet.<BR>
     * When the endpoint disconnects using disconnect(), a will won't send.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718090<BR>
     */
    void async_disconnect(async_handler_t const& func = async_handler_t()) {
        if (connected_ && mqtt_connected_) {
            disconnect_requested_ = true;
            async_send_disconnect(func);
        }
    }

    // packet_id manual setting version

    /**
     * @brief Publish QoS1 with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         contents doesn't publish, otherwise return true and contents publish.
     */
    bool async_publish_at_least_once(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::string const& contents,
        bool retain = false,
        async_handler_t const& func = async_handler_t()) {
        if (register_packet_id(packet_id)) {
            acquired_async_publish_at_least_once(packet_id, topic_name, contents, retain, func);
            return true;
        }
        return false;
    }

    /**
     * @brief Publish QoS1 with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         contents doesn't publish, otherwise return true and contents publish.
     */
    bool async_publish_at_least_once(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        bool retain = false,
        async_handler_t const& func = async_handler_t()) {
        if (register_packet_id(packet_id)) {
            acquired_async_publish_at_least_once(packet_id, topic_name, contents, life_keeper, retain, func);
            return true;
        }
        return false;
    }

    /**
     * @brief Publish QoS2 with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         contents doesn't publish, otherwise return true and contents publish.
     */
    bool async_publish_exactly_once(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::string const& contents,
        bool retain = false,
        async_handler_t const& func = async_handler_t()) {
        if (register_packet_id(packet_id)) {
            acquired_async_publish_exactly_once(packet_id, topic_name, contents, retain, func);
            return true;
        }
        return false;
    }

    /**
     * @brief Publish QoS2 with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         contents doesn't publish, otherwise return true and contents publish.
     */
    bool async_publish_exactly_once(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        bool retain = false,
        async_handler_t const& func = async_handler_t()) {
        if (register_packet_id(packet_id)) {
            acquired_async_publish_exactly_once(packet_id, topic_name, contents, life_keeper, retain, func);
            return true;
        }
        return false;
    }

    /**
     * @brief Publish with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         contents don't publish, otherwise return true and contents publish.
     */
    bool async_publish(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::string const& contents,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false,
        async_handler_t const& func = async_handler_t()) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        if (register_packet_id(packet_id)) {
            acquired_async_publish(packet_id, topic_name, contents, qos, retain, func);
            return true;
        }
        return false;
    }

    /**
     * @brief Publish with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         contents don't publish, otherwise return true and contents publish.
     */
    bool async_publish(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false,
        async_handler_t const& func = async_handler_t()) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        if (register_packet_id(packet_id)) {
            acquired_async_publish(packet_id, topic_name, contents, life_keeper, qos, retain, func);
            return true;
        }
        return false;
    }

    /**
     * @brief Publish as dup with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         contents don't publish, otherwise return true and contents publish.
     */
    bool async_publish_dup(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::string const& contents,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false,
        async_handler_t const& func = async_handler_t()) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        if (register_packet_id(packet_id)) {
            acquired_async_publish_dup(packet_id, topic_name, contents, qos, retain, func);
            return true;
        }
        return false;
    }

    /**
     * @brief Publish as dup with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         contents don't publish, otherwise return true and contents publish.
     */
    bool async_publish_dup(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false,
        async_handler_t const& func = async_handler_t()) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        if (register_packet_id(packet_id)) {
            acquired_async_publish_dup(packet_id, topic_name, contents, life_keeper, qos, retain, func);
            return true;
        }
        return false;
    }

    /**
     * @brief Subscribe with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param args
     *        args should be zero or more pairs of topic_name and qos.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't subscribe, otherwise return true and subscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066<BR>
     */
    template <typename... Args>
    typename std::enable_if<
        sizeof...(Args) != 0,
        bool
    >::type
    async_subscribe(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::uint8_t qos, Args&&... args) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        if (register_packet_id(packet_id)) {
            acquired_async_subscribe(packet_id, topic_name, qos, std::forward<Args>(args)...);
            return true;
        }
        return false;
    }

    /**
     * @brief Subscribe with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param args
     *        args should be zero or more pairs of topic_name and qos.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't subscribe, otherwise return true and subscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066<BR>
     */
    template <typename... Args>
    typename std::enable_if<
        sizeof...(Args) != 0,
        bool
    >::type
    async_subscribe(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        std::uint8_t qos, Args&&... args) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        if (register_packet_id(packet_id)) {
            acquired_async_subscribe(packet_id, topic_name, qos, std::forward<Args>(args)...);
            return true;
        }
        return false;
    }

    /**
     * @brief Subscribe
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param func A callback function that is called when async operation will finish.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't subscribe, otherwise return true and subscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    bool async_subscribe(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::uint8_t qos,
        async_handler_t const& func = async_handler_t()) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        if (register_packet_id(packet_id)) {
            acquired_async_subscribe(packet_id, topic_name, qos, func);
            return true;
        }
        return false;
    }

    /**
     * @brief Subscribe
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param func A callback function that is called when async operation will finish.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't subscribe, otherwise return true and subscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    bool async_subscribe(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        std::uint8_t qos,
        async_handler_t const& func = async_handler_t()) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        if (register_packet_id(packet_id)) {
            acquired_async_subscribe(packet_id, topic_name, qos, func);
            return true;
        }
        return false;
    }

    /**
     * @brief Subscribe
     * @param packet_id
     *        packet identifier
     * @param params A collection of the pair of topic_name and qos to subscribe.
     * @param func A callback function that is called when async operation will finish.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't subscribe, otherwise return true and subscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    bool async_subscribe(
        packet_id_t packet_id,
        std::vector<std::tuple<std::string, std::uint8_t>> const& params,
        async_handler_t const& func = async_handler_t()) {
        if (register_packet_id(packet_id)) {
            acquired_async_subscribe(packet_id, params, func);
            return true;
        }
        return false;
    }

    /**
     * @brief Subscribe
     * @param packet_id
     *        packet identifier
     * @param params A collection of the pair of topic_name and qos to subscribe.
     * @param func A callback function that is called when async operation will finish.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't subscribe, otherwise return true and subscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    bool async_subscribe(
        packet_id_t packet_id,
        std::vector<std::tuple<as::const_buffer, std::uint8_t>> const& params,
        async_handler_t const& func = async_handler_t()) {
        if (register_packet_id(packet_id)) {
            acquired_async_subscribe(packet_id, params, func);
            return true;
        }
        return false;
    }

    /**
     * @brief Unsubscribe with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to subscribe
     * @param args
     *        args should be some topic_names to unsubscribe, <BR>
     *        and the last one is a callback function that is called when async operation will finish.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't unsubscribe, otherwise return true and unsubscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    template <typename... Args>
    typename std::enable_if<
        sizeof...(Args) != 0,
        bool
    >::type
    async_unsubscribe(
        packet_id_t packet_id,
        std::string const& topic_name,
        Args&&... args) {
        if (register_packet_id(packet_id)) {
            acquired_async_unsubscribe(packet_id, topic_name, std::forward<Args>(args)...);
            return true;
        }
        return false;
    }

    /**
     * @brief Unsubscribe with a manual set packet identifier
     * @param packet_id
     *        packet identifier
     * @param topic_name
     *        A topic name to subscribe
     * @param args
     *        args should be some topic_names to unsubscribe, <BR>
     *        and the last one is a callback function that is called when async operation will finish.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't unsubscribe, otherwise return true and unsubscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    template <typename... Args>
    typename std::enable_if<
        sizeof...(Args) != 0,
        bool
    >::type
    async_unsubscribe(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        Args&&... args) {
        if (register_packet_id(packet_id)) {
            acquired_async_unsubscribe(packet_id, topic_name, std::forward<Args>(args)...);
            return true;
        }
        return false;
    }

    /**
     * @brief Unsubscribe
     * @param packet_id
     *        packet identifier
     * @param params
     *        A collection of the topic name to unsubscribe
     * @param func A callback function that is called when async operation will finish.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't unsubscribe, otherwise return true and unsubscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    bool async_unsubscribe(
        packet_id_t packet_id,
        std::vector<std::string> const& params,
        async_handler_t const& func = async_handler_t()) {
        if (register_packet_id(packet_id)) {
            acquired_async_unsubscribe(packet_id, params, func);
            return true;
        }
        return false;
    }

    /**
     * @brief Unsubscribe
     * @param packet_id
     *        packet identifier
     * @param params
     *        A collection of the topic name to unsubscribe
     * @param func A callback function that is called when async operation will finish.
     * @return If packet_id is used in the publishing/subscribing sequence, then returns false and
     *         doesn't unsubscribe, otherwise return true and unsubscribes.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    bool async_unsubscribe(
        packet_id_t packet_id,
        std::vector<as::const_buffer> const& params,
        async_handler_t const& func = async_handler_t()) {
        if (register_packet_id(packet_id)) {
            acquired_async_unsubscribe(packet_id, params, func);
            return true;
        }
        return false;
    }

    // packet_id has already been acquired version

    /**
     * @brief Publish QoS1 with a manual set packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     */
    void acquired_async_publish_at_least_once(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::string const& contents,
        bool retain = false,
        async_handler_t const& func = async_handler_t(),
        boost::optional<std::vector<v5::property_variant>> props = boost::none
    ) {

        auto sp_topic_name = std::make_shared<std::string>(topic_name);
        auto sp_contents = std::make_shared<std::string>(contents);

        async_send_publish(
            as::buffer(*sp_topic_name),
            qos::at_least_once,
            retain,
            false,
            packet_id,
            std::move(props),
            as::buffer(*sp_contents),
            func,
            [sp_topic_name, sp_contents] {}
        );
    }

    /**
     * @brief Publish QoS1 with a manual set packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper
     *        The function for keeping topic_name and contents life.
     *        It is usually a lambda expression that captures shared_ptr of topic_name and contents.
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     */
    void acquired_async_publish_at_least_once(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        bool retain = false,
        async_handler_t const& func = async_handler_t(),
        boost::optional<std::vector<v5::property_variant>> props = boost::none
    ) {

        async_send_publish(
            topic_name,
            qos::at_least_once,
            retain,
            false,
            packet_id,
            std::move(props),
            contents,
            func,
            life_keeper
        );
    }

    /**
     * @brief Publish QoS2 with a manual set packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     */
    void acquired_async_publish_exactly_once(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::string const& contents,
        bool retain = false,
        async_handler_t const& func = async_handler_t(),
        boost::optional<std::vector<v5::property_variant>> props = boost::none
    ) {

        auto sp_topic_name = std::make_shared<std::string>(topic_name);
        auto sp_contents = std::make_shared<std::string>(contents);

        async_send_publish(
            as::buffer(*sp_topic_name),
            qos::exactly_once,
            retain,
            false,
            packet_id,
            std::move(props),
            as::buffer(*sp_contents),
            func,
            [sp_topic_name, sp_contents] {}
        );
    }

    /**
     * @brief Publish QoS2 with a manual set packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     */
    void acquired_async_publish_exactly_once(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        bool retain = false,
        async_handler_t const& func = async_handler_t(),
        boost::optional<std::vector<v5::property_variant>> props = boost::none
    ) {

        async_send_publish(
            topic_name,
            qos::exactly_once,
            retain,
            false,
            packet_id,
            std::move(props),
            contents,
            func,
            life_keeper
        );
    }

    /**
     * @brief Publish with a manual set packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     *        If qos == qos::at_most_once, packet_id must be 0. But not checked in release mode due to performance.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     */
    void acquired_async_publish(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::string const& contents,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false,
        async_handler_t const& func = async_handler_t(),
        boost::optional<std::vector<v5::property_variant>> props = boost::none
    ) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        BOOST_ASSERT((qos == qos::at_most_once && packet_id == 0) || (qos != qos::at_most_once && packet_id != 0));

        auto sp_topic_name = std::make_shared<std::string>(topic_name);
        auto sp_contents = std::make_shared<std::string>(contents);

        async_send_publish(
            as::buffer(*sp_topic_name),
            qos,
            retain,
            false,
            packet_id,
            std::move(props),
            as::buffer(*sp_contents),
            func,
            [sp_topic_name, sp_contents] {}
        );
    }

    /**
     * @brief Publish with a manual set packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     *        If qos == qos::at_most_once, packet_id must be 0. But not checked in release mode due to performance.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     */
    void acquired_async_publish(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false,
        async_handler_t const& func = async_handler_t(),
        boost::optional<std::vector<v5::property_variant>> props = boost::none
    ) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        BOOST_ASSERT((qos == qos::at_most_once && packet_id == 0) || (qos != qos::at_most_once && packet_id != 0));

        async_send_publish(
            topic_name,
            qos,
            retain,
            false,
            packet_id,
            std::move(props),
            contents,
            func,
            life_keeper
        );
    }

    /**
     * @brief Publish as dup with a manual set packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     *        If qos == qos::at_most_once, packet_id must be 0. But not checked in release mode due to performance.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     */
    void acquired_async_publish_dup(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::string const& contents,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false,
        async_handler_t const& func = async_handler_t(),
        boost::optional<std::vector<v5::property_variant>> props = boost::none
    ) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        BOOST_ASSERT((qos == qos::at_most_once && packet_id == 0) || (qos != qos::at_most_once && packet_id != 0));

        auto sp_topic_name = std::make_shared<std::string>(topic_name);
        auto sp_contents = std::make_shared<std::string>(contents);

        async_send_publish(
            as::buffer(*sp_topic_name),
            qos,
            retain,
            true,
            packet_id,
            std::move(props),
            as::buffer(*sp_contents),
            func,
            [sp_topic_name, sp_contents] {}
        );
    }

    /**
     * @brief Publish as dup with a manual set packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     *        If qos == qos::at_most_once, packet_id must be 0. But not checked in release mode due to performance.
     * @param topic_name
     *        A topic name to publish
     * @param contents
     *        The contents to publish
     * @param life_keeper the function that is keeping topic_name and contents lifetime.
     * @param qos
     *        mqtt::qos
     * @param retain
     *        A retain flag. If set it to true, the contents is retained.<BR>
     *        See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038<BR>
     *        3.3.1.3 RETAIN
     * @param func A callback function that is called when async operation will finish.
     */
    void acquired_async_publish_dup(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        as::const_buffer const& contents,
        life_keeper_t const& life_keeper,
        std::uint8_t qos = qos::at_most_once,
        bool retain = false,
        async_handler_t const& func = async_handler_t(),
        boost::optional<std::vector<v5::property_variant>> props = boost::none
    ) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        BOOST_ASSERT((qos == qos::at_most_once && packet_id == 0) || (qos != qos::at_most_once && packet_id != 0));

        async_send_publish(
            topic_name,
            qos,
            retain,
            true,
            packet_id,
            std::move(props),
            contents,
            func,
            life_keeper
        );
    }

    /**
     * @brief Subscribe with a manual set packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param args
     *        args should be some pairs of topic_name and qos to subscribe, <BR>
     *        and the last one is a callback function that is called when async operation will finish.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066<BR>
     */
    template <typename... Args>
    typename std::enable_if<
        sizeof...(Args) != 0,
        void
    >::type
    acquired_async_subscribe(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::uint8_t qos, Args&&... args) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        return acquired_async_subscribe_imp(packet_id, topic_name, qos, std::forward<Args>(args)...);
    }

    /**
     * @brief Subscribe with a manual set packet identifier
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param args
     *        args should be some pairs of topic_name and qos to subscribe, <BR>
     *        and the last one is a callback function that is called when async operation will finish.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066<BR>
     */
    template <typename... Args>
    typename std::enable_if<
        sizeof...(Args) != 0,
        void
    >::type
    acquired_async_subscribe(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        std::uint8_t qos, Args&&... args) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        return acquired_async_subscribe_imp(packet_id, topic_name, qos, std::forward<Args>(args)...);
    }

    /**
     * @brief Subscribe
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param func A callback function that is called when async operation will finish.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    void acquired_async_subscribe(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::uint8_t qos,
        async_handler_t const& func = async_handler_t()) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);

        std::vector<std::tuple<as::const_buffer, std::uint8_t>> params;
        std::vector<std::shared_ptr<std::string>> life_keepers;

        params.reserve(1);
        async_send_subscribe(
            params,
            life_keepers,
            packet_id,
            topic_name,
            qos,
            func
        );
    }

    /**
     * @brief Subscribe
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name
     *        A topic name to subscribe
     * @param qos
     *        mqtt::qos
     * @param func A callback function that is called when async operation will finish.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    void acquired_async_subscribe(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        std::uint8_t qos,
        async_handler_t const& func = async_handler_t()) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);

        std::vector<std::tuple<as::const_buffer, std::uint8_t>> params;
        std::vector<std::shared_ptr<std::string>> life_keepers;

        params.reserve(1);
        async_send_subscribe(
            params,
            life_keepers,
            packet_id,
            topic_name,
            qos,
            func
        );
    }

    /**
     * @brief Subscribe
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param params A collection of the pair of topic_name and qos to subscribe.
     * @param func A callback function that is called when async operation will finish.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    void acquired_async_subscribe(
        packet_id_t packet_id,
        std::vector<std::tuple<std::string, std::uint8_t>> const& params,
        async_handler_t const& func = async_handler_t()) {

        std::vector<std::tuple<as::const_buffer, std::uint8_t>> cb_params;
        cb_params.reserve(params.size());

        std::vector<std::shared_ptr<std::string>> life_keepers;
        life_keepers.reserve(params.size());

        for (auto const& e : params) {
            life_keepers.emplace_back(std::make_shared<std::string>(std::get<0>(e)));
            cb_params.emplace_back(
                as::buffer(*life_keepers.back()),
                std::get<1>(e));
        }
        async_send_subscribe(
            cb_params,
            life_keepers,
            packet_id,
            func
        );
    }

    /**
     * @brief Subscribe
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param params A collection of the pair of topic_name and qos to subscribe.
     * @param func A callback function that is called when async operation will finish.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    void acquired_async_subscribe(
        packet_id_t packet_id,
        std::vector<std::tuple<as::const_buffer, std::uint8_t>> const& params,
        async_handler_t const& func = async_handler_t()) {

        std::vector<std::shared_ptr<std::string>> life_keepers;

        async_send_subscribe(
            params,
            life_keepers,
            packet_id,
            func
        );
    }

    /**
     * @brief Unsubscribe
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name topic_name
     * @param args
     *        args should be some topic_names, <BR>
     *        and the last one is a callback function that is called when async operation will finish.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    template <typename... Args>
    typename std::enable_if<
        sizeof...(Args) != 0
    >::type
    acquired_async_unsubscribe(
        packet_id_t packet_id,
        std::string const& topic_name,
        Args&&... args) {
        acquired_async_unsubscribe_imp(packet_id, topic_name, std::forward<Args>(args)...);
    }

    /**
     * @brief Unsubscribe
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param topic_name topic_name
     * @param args
     *        args should be some topic_names, <BR>
     *        and the last one is a callback function that is called when async operation will finish.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    template <typename... Args>
    typename std::enable_if<
        sizeof...(Args) != 0
    >::type
    acquired_async_unsubscribe(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        Args&&... args) {
        acquired_async_unsubscribe_imp(packet_id, topic_name, std::forward<Args>(args)...);
    }

    /**
     * @brief Unsubscribe
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param params
     *        A collection of the topic name to unsubscribe
     * @param func A callback function that is called when async operation will finish.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    void acquired_async_unsubscribe(
        packet_id_t packet_id,
        std::vector<std::string> const& params,
        async_handler_t const& func = async_handler_t()) {

        std::vector<as::const_buffer> cb_params;
        cb_params.reserve(params.size());

        std::vector<std::shared_ptr<std::string>> life_keepers;
        life_keepers.reserve(params.size());

        for (auto const& e : params) {
            life_keepers.emplace_back(std::make_shared<std::string>(e));
            cb_params.emplace_back(as::buffer(*life_keepers.back()));
        }

        async_send_unsubscribe(
            cb_params,
            life_keepers,
            packet_id,
            func
        );
    }

    /**
     * @brief Unsubscribe
     * @param packet_id
     *        packet identifier. It should be acquired by acquire_unique_packet_id, or register_packet_id.
     *        The ownership of  the packet_id moves to the library.
     * @param params
     *        A collection of the topic name to unsubscribe
     * @param func A callback function that is called when async operation will finish.
     * You can subscribe multiple topics all at once.<BR>
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    void acquired_async_unsubscribe(
        packet_id_t packet_id,
        std::vector<as::const_buffer> const& params,
        async_handler_t const& func = async_handler_t()) {

        std::vector<std::shared_ptr<std::string>> life_keepers;
        async_send_unsubscribe(
            params,
            life_keepers,
            packet_id,
            func
        );
    }

    /**
     * @brief Send pingreq packet.
     * @param func A callback function that is called when async operation will finish.
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718066
     */
    void async_pingreq(async_handler_t const& func = async_handler_t()) {
        if (connected_ && mqtt_connected_) async_send_pingreq(func);
    }

    /**
     * @brief Send pingresp packet. This function is for broker.
     * @param func A callback function that is called when async operation will finish.
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718086
     */
    void async_pingresp(async_handler_t const& func = async_handler_t()) {
        async_send_pingresp(func);
    }


    /**
     * @brief Send connect packet.
     * @param keep_alive_sec See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349238
     * @param func A callback function that is called when async operation will finish.
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718028
     */
    void async_connect(
        std::uint16_t keep_alive_sec,
        async_handler_t const& func = async_handler_t()) {
        connect_requested_ = true;
        async_send_connect(keep_alive_sec, func);
    }

    /**
     * @brief Send connack packet. This function is for broker.
     * @param session_present See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349255
     * @param return_code See connect_return_code.hpp and http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc385349256
     * @param func A callback function that is called when async operation will finish.
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718033
     */
    void async_connack(
        bool session_present,
        std::uint8_t return_code,
        async_handler_t const& func = async_handler_t()) {
        async_send_connack(session_present, return_code, func);
    }

    /**
     * @brief Send puback packet.
     * @param packet_id packet id corresponding to publish
     * @param func A callback function that is called when async operation will finish.
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718043
     */
    void async_puback(packet_id_t packet_id, async_handler_t const& func = async_handler_t()) {
        async_send_puback(packet_id, func);
    }

    /**
     * @brief Send pubrec packet.
     * @param packet_id packet id corresponding to publish
     * @param func A callback function that is called when async operation will finish.
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718048
     */
    void async_pubrec(packet_id_t packet_id, async_handler_t const& func = async_handler_t()) {
        async_send_pubrec(packet_id, func);
    }

    /**
     * @brief Send pubrel packet.
     * @param packet_id packet id corresponding to publish
     * @param func A callback function that is called when async operation will finish.
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718053
     */
    void async_pubrel(packet_id_t packet_id, async_handler_t const& func = async_handler_t()) {
        async_send_pubrel(packet_id, func);
    }

    /**
     * @brief Send pubcomp packet.
     * @param packet_id packet id corresponding to publish
     * @param func A callback function that is called when async operation will finish.
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718058
     */
    void async_pubcomp(packet_id_t packet_id, async_handler_t const& func = async_handler_t()) {
        async_send_pubcomp(packet_id, func);
    }

    /**
     * @brief Send suback packet. This function is for broker.
     * @param packet_id packet id corresponding to subscribe
     * @param qos qos
     * @param args
     *        args should be some qos corresponding to subscribe, <BR>
     *        and the last one is a callback function that is called when async operation will finish.
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718068
     */
    template <typename... Args>
    typename std::enable_if<
        sizeof...(Args) != 0
    >::type
    async_suback(
        packet_id_t packet_id,
        std::uint8_t qos, Args&&... args) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        async_suback_imp(packet_id, qos, std::forward<Args>(args)...);
    }

    /**
     * @brief Send suback packet. This function is for broker.
     * @param packet_id packet id corresponding to subscribe
     * @param qos adjusted qos
     * @param func A callback function that is called when async operation will finish.
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718068
     */
    void async_suback(
        packet_id_t packet_id,
        std::uint8_t qos,
        async_handler_t const& func = async_handler_t()) {
        BOOST_ASSERT(qos == qos::at_most_once || qos::at_least_once || qos::exactly_once);
        std::vector<std::uint8_t> params;
        async_send_suback(params, packet_id, qos, func);
    }

    /**
     * @brief Send suback packet. This function is for broker.
     * @param packet_id packet id corresponding to subscribe
     * @param qoss a collection of adjusted qos
     * @param func A callback function that is called when async operation will finish.
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718068
     */
    void async_suback(
        packet_id_t packet_id,
        std::vector<std::uint8_t> const& qoss,
        async_handler_t const& func = async_handler_t()) {
        async_send_suback(qoss, packet_id, func);
    }

    /**
     * @brief Send unsuback packet. This function is for broker.
     * @param packet_id packet id corresponding to unsubscribe
     * @param func A callback function that is called when async operation will finish.
     * See http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718077
     */
    void async_unsuback(
        packet_id_t packet_id,
        async_handler_t const& func = async_handler_t()) {
        async_send_unsuback(packet_id, func);
    }

    /**
     * @brief Clear storead publish message that has packet_id.
     * @param packet_id packet id corresponding to stored publish
     */
    void clear_stored_publish(packet_id_t packet_id) {
        LockGuard<Mutex> lck (store_mtx_);
        auto& idx = store_.template get<tag_packet_id>();
        auto r = idx.equal_range(packet_id);
        idx.erase(std::get<0>(r), std::get<1>(r));
        packet_id_.erase(packet_id);
    }

    /**
     * @brief Get Socket unique_ptr reference.
     * @return refereence of Socket unique_ptr
     */
    std::unique_ptr<Socket>& socket() {
        return socket_;
    }

    /**
     * @brief Get Socket unique_ptr const reference.
     * @return const refereence of Socket unique_ptr
     */
    std::unique_ptr<Socket> const& socket() const {
        return socket_;
    }


    /**
     * @brief Apply f to stored messages.
     * @param f applying function. f should be void(char const*, std::size_t)
     */
    void for_each_store(std::function<void(char const*, std::size_t)> const& f) {
        LockGuard<Mutex> lck (store_mtx_);
        auto& idx = store_.template get<tag_seq>();
        for (auto const & e : idx) {
            auto const& m = e.message();
            auto cb = continuous_buffer(m);
            f(cb.data(), cb.size());
        }
    }

    /**
     * @brief Apply f to stored messages.
     * @param f applying function. f should be void(message_variant const&)
     */
    void for_each_store(std::function<void(message_variant const&)> const& f) {
        LockGuard<Mutex> lck (store_mtx_);
        auto& idx = store_.template get<tag_seq>();
        for (auto const & e : idx) {
            f(e.message());
        }
    }

    // manual packet_id management for advanced users

    /**
     * @brief Acquire the new unique packet id.
     *        If all packet ids are already in use, then throw packet_id_exhausted_error exception.
     *        After acquiring the packet id, you can call acuired_* functions.
     *        The ownership of packet id is moved to the library.
     *        Or you can call release_packet_id to release it.
     * @return packet id
     */
    packet_id_t acquire_unique_packet_id() {
        LockGuard<Mutex> lck (store_mtx_);
        if (packet_id_.size() == std::numeric_limits<packet_id_t>::max()) throw packet_id_exhausted_error();
        if (packet_id_master_ == std::numeric_limits<packet_id_t>::max()) {
            packet_id_master_ = 1;
        }
        else {
            ++packet_id_master_;
        }
        auto ret = packet_id_.insert(packet_id_master_);
        if (ret.second) return packet_id_master_;

        auto last = packet_id_.end();
        auto e = last;
        --last;

        if (*last != std::numeric_limits<packet_id_t>::max()) {
            packet_id_master_ = *last + 1;
            packet_id_.insert(e, packet_id_master_);
            return packet_id_master_;
        }

        auto b = packet_id_.begin();
        auto prev = *b;
        if (prev != 1) {
            packet_id_master_ = 1;
            packet_id_.insert(b, packet_id_master_);
            return packet_id_master_;
        }
        ++b;
        while (*b - 1 == prev && b != e) {
            prev = *b;
            ++b;
        }
        packet_id_master_ = prev + 1;
        packet_id_.insert(b, packet_id_master_);
        return packet_id_master_;
    }

    /**
     * @brief Register packet_id to the library.
     *        After registering the packet_id, you can call acuired_* functions.
     *        The ownership of packet id is moved to the library.
     *        Or you can call release_packet_id to release it.
     * @return If packet_id is successfully registerd then return true, otherwise return false.
     */
    bool register_packet_id(packet_id_t packet_id) {
        if (packet_id == 0) return false;
        LockGuard<Mutex> lck (store_mtx_);
        return packet_id_.insert(packet_id).second;
    }

    /**
     * @brief Release packet_id.
     * @param packet_id packet id to release.
     *                   only the packet_id gotten by acquire_unique_packet_id, or
     *                   register_packet_id is permitted.
     * @return If packet_id is successfully released then return true, otherwise return false.
     */
    bool release_packet_id(packet_id_t packet_id) {
        LockGuard<Mutex> lck (store_mtx_);
        return packet_id_.erase(packet_id);
    }

    /**
     * @brief Restore serialized publish and pubrel messages.
     *        This function shouold be called before connect.
     * @param packet_id packet id of the message
     * @param b         iterator begin of the message
     * @param e         iterator end of the message
     */
    template <typename Iterator>
    typename std::enable_if<std::is_convertible<typename Iterator::value_type, char>::value>::type
    restore_serialized_message(packet_id_t /*packet_id*/, Iterator b, Iterator e) {
        if (b == e) return;

        auto fixed_header = *b;
        switch (get_control_packet_type(fixed_header)) {
        case control_packet_type::publish: {
            auto sp = std::make_shared<basic_publish_message<PacketIdBytes>>(b, e);
            restore_serialized_message(*sp, [sp] {});
        } break;
        case control_packet_type::pubrel: {
            restore_serialized_message(basic_pubrel_message<PacketIdBytes>(b, e));
        } break;
        default:
            throw protocol_error();
            break;
        }
    }

    /**
     * @brief Restore serialized publish message.
     *        This function shouold be called before connect.
     * @param msg         publish message.
     * @param life_keeper the function that keeps the msg lifetime.
     */
    void restore_serialized_message(basic_publish_message<PacketIdBytes> msg, life_keeper_t life_keeper) {
        auto packet_id = msg.packet_id();
        auto qos = msg.qos();
        LockGuard<Mutex> lck (store_mtx_);
        if (packet_id_.insert(packet_id).second) {
            auto ret = store_.emplace(
                packet_id,
                qos == qos::at_least_once ? control_packet_type::puback
                                          : control_packet_type::pubrec,
                std::move(msg),
                std::move(life_keeper)
            );
            BOOST_ASSERT(ret.second);
        }
    }

    /**
     * @brief Restore serialized pubrel message.
     *        This function shouold be called before connect.
     * @param msg pubrel message.
     */
    void restore_serialized_message(basic_pubrel_message<PacketIdBytes> msg) {
        auto packet_id = msg.packet_id();
        LockGuard<Mutex> lck (store_mtx_);
        if (packet_id_.insert(packet_id).second) {
            auto ret = store_.emplace(
                packet_id,
                control_packet_type::pubcomp,
                std::move(msg),
                []{}
            );
            BOOST_ASSERT(ret.second);
        }
    }

    /**
     * @brief Restore serialized publish and pubrel messages.
     *        This function shouold be called before connect.
     * @param packet_id packet id of the message
     * @param b         iterator begin of the message
     * @param e         iterator end of the message
     */
    template <typename Iterator>
    typename std::enable_if<std::is_convertible<typename Iterator::value_type, char>::value>::type
    restore_v5_serialized_message(packet_id_t /*packet_id*/, Iterator b, Iterator e) {
        if (b == e) return;

        auto fixed_header = *b;
        switch (get_control_packet_type(fixed_header)) {
        case control_packet_type::publish: {
            auto sp = std::make_shared<v5::basic_publish_message<PacketIdBytes>>(b, e);
            restore_serialized_message(*sp, [sp] {});
        } break;
        case control_packet_type::pubrel: {
            restore_serialized_message(v5::basic_pubrel_message<PacketIdBytes>(b, e));
        } break;
        default:
            throw protocol_error();
            break;
        }
    }

    /**
     * @brief Restore serialized publish message.
     *        This function shouold be called before connect.
     * @param msg         publish message.
     * @param life_keeper the function that keeps the msg lifetime.
     */
    void restore_v5_serialized_message(v5::basic_publish_message<PacketIdBytes> msg, life_keeper_t life_keeper) {
        auto packet_id = msg.packet_id();
        auto qos = msg.qos();
        LockGuard<Mutex> lck (store_mtx_);
        if (packet_id_.insert(packet_id).second) {
            auto ret = store_.emplace(
                packet_id,
                qos == qos::at_least_once ? control_packet_type::puback
                                          : control_packet_type::pubrec,
                std::move(msg),
                std::move(life_keeper)
            );
            BOOST_ASSERT(ret.second);
        }
    }

    /**
     * @brief Restore serialized pubrel message.
     *        This function shouold be called before connect.
     * @param msg pubrel message.
     */
    void restore_v5_serialized_message(v5::basic_pubrel_message<PacketIdBytes> msg) {
        auto packet_id = msg.packet_id();
        LockGuard<Mutex> lck (store_mtx_);
        if (packet_id_.insert(packet_id).second) {
            auto ret = store_.emplace(
                packet_id,
                control_packet_type::pubcomp,
                std::move(msg),
                []{}
            );
            BOOST_ASSERT(ret.second);
        }
    }

    /**
     * @brief Check connection status
     * @return current connection status
     */
    bool connected() const {
        return connected_ && mqtt_connected_;
    }

    /**
     * @brief Set custom mqtt_message_processed_handler.
     *        The default setting is calling async_read_control_packet_type().
     *        (See endpoint() constructor).
     *        The typical usecase of this function is delaying the next
     *        message reading timing.
     *        In order to do that
     *        1) store func parameter of the mqtt_message_processed_handler.
     *        2) call async_read_next_message with the stored func if
     *           you are ready to read the next mqtt message.
     * @param h mqtt_message_processed_handler.
     */
    void set_mqtt_message_processed_handler(
        mqtt_message_processed_handler const& h = mqtt_message_processed_handler()) {
        if (h) {
            h_mqtt_message_processed_ = h;
        }
        else {
            h_mqtt_message_processed_ =
                [this]
                (async_handler_t const& func) {
                async_read_control_packet_type(func);
            };
        }
    }

    /**
     * @brief Trigger next mqtt message manually.
     *        If you call this function, you need to set manual receive mode
     *        using set_auto_next_read(false);
     */
    void async_read_next_message(async_handler_t const& func) {
        async_read_control_packet_type(func);
    }

protected:
    void async_read_control_packet_type(async_handler_t const& func) {
        auto self = this->shared_from_this();
        async_read(
            *socket_,
            as::buffer(&buf_, 1),
            [this, self, func](
                boost::system::error_code const& ec,
                std::size_t bytes_transferred){
                if (handle_close_or_error(ec)) {
                    if (func) func(ec);
                    return;
                }
                if (bytes_transferred != 1) {
                    if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                    return;
                }
                handle_control_packet_type(func);
            }
        );
    }

    bool handle_close_or_error(boost::system::error_code const& ec) {
        if (!ec) return false;
        if (connected_) {
            connected_ = false;
            mqtt_connected_ = false;
            shutdown_from_server(*socket_);
        }
        if (ec == as::error::eof ||
            ec == as::error::connection_reset
#if defined(MQTT_USE_WS)
            ||
            ec == boost::beast::websocket::error::closed
#endif // defined(MQTT_USE_WS)
#if !defined(MQTT_NO_TLS)
            ||
#if defined(SSL_R_SHORT_READ)
            ERR_GET_REASON(ec.value()) == SSL_R_SHORT_READ
#else  // defined(SSL_R_SHORT_READ)
            ERR_GET_REASON(ec.value()) == boost::asio::ssl::error::stream_truncated
#endif // defined(SSL_R_SHORT_READ)
#endif // defined(MQTT_NO_TLS)
        ) {
            if (disconnect_requested_) {
                disconnect_requested_ = false;
                connect_requested_ = false;
                handle_close();
                return true;
            }
        }
        disconnect_requested_ = false;
        connect_requested_ = false;
        handle_error(ec);
        return true;
    }

    void set_connect() {
        connected_ = true;
    }

private:
    std::size_t connect_remaining_length() const {
        std::size_t remaining_length = 10; // variable header
        if (user_name_) {
            remaining_length += 2 + user_name_.get().size();
        }
        if (password_) {
            remaining_length += 2 + password_.get().size();
        }
        remaining_length += 2 + client_id_.size();
        if (will_) {
            remaining_length += 2 + will_.get().topic().size();
            remaining_length += 2 + will_.get().message().size();
        }
        return remaining_length;
    }

    static std::size_t publish_remaining_length(
        as::const_buffer const& topic_name,
        std::uint8_t qos,
        as::const_buffer const& payload) {
        return
            2                      // topic name length
            + get_size(topic_name) // topic name
            + get_size(payload)    // payload
            + [&] {
                  if (qos == qos::at_least_once || qos == qos::exactly_once) {
                      return PacketIdBytes;
                  }
                  else {
                      return 0;
                  }
              }();
    }

    static std::size_t subscribe_remaining_length(
        std::vector<std::tuple<as::const_buffer, std::uint8_t>> const& params
    ) {
        std::size_t remaining_length = PacketIdBytes;
        for (auto const& e : params) {
            remaining_length += 2 + get_size(std::get<0>(e)) + 1;
        }
        return remaining_length;
    }

    static std::size_t unsubscribe_remaining_length(
        std::vector<as::const_buffer> const& params
    ) {
        std::size_t remaining_length = PacketIdBytes;
        for (auto const& e : params) {
            remaining_length += 2 + get_size(e);
        }
        return remaining_length;
    }

    template <typename T>
    void shutdown_from_client(T& socket) {
        boost::system::error_code ec;
        socket.lowest_layer().close(ec);
    }

    template <typename T>
    void shutdown_from_server(T& socket) {
        boost::system::error_code ec;
        socket.close(ec);
    }

#if defined(MQTT_USE_WS)
    template <typename T, typename S>
    void shutdown_from_server(ws_endpoint<T, S>& /*socket*/) {
    }
#endif // defined(MQTT_USE_WS)

#if !defined(MQTT_NO_TLS)
    template <typename T>
    void shutdown_from_client(as::ssl::stream<T>& socket) {
        boost::system::error_code ec;
        socket.lowest_layer().close(ec);
    }
    template <typename T>
    void shutdown_from_server(as::ssl::stream<T>& socket) {
        boost::system::error_code ec;
        socket.lowest_layer().close(ec);
    }
#if defined(MQTT_USE_WS)
    template <typename T, typename S>
    void shutdown_from_client(ws_endpoint<as::ssl::stream<T>, S>& socket) {
        boost::system::error_code ec;
        socket.lowest_layer().close(ec);
    }
    template <typename T, typename S>
    void shutdown_from_server(ws_endpoint<as::ssl::stream<T>, S>& /*socket*/) {
    }
#endif // defined(MQTT_USE_WS)
#endif // defined(MQTT_NO_TLS)


    template <typename... Args>
    typename std::enable_if<
        std::is_convertible<
            typename std::tuple_element<sizeof...(Args) - 1, std::tuple<Args...>>::type,
            async_handler_t
        >::value
    >::type
    acquired_async_subscribe_imp(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::uint8_t qos, Args&&... args) {

        std::vector<std::tuple<as::const_buffer, std::uint8_t>> params;
        params.reserve((sizeof...(args) + 2) / 2);

        std::vector<std::shared_ptr<std::string>> life_keepers;
        life_keepers.reserve((sizeof...(args) + 2) / 2);

        async_send_subscribe(
            params,
            life_keepers,
            packet_id,
            topic_name,
            qos,
            std::forward<Args>(args)...
        );
    }

    template <typename... Args>
    typename std::enable_if<
        std::is_convertible<
            typename std::tuple_element<sizeof...(Args) - 1, std::tuple<Args...>>::type,
            async_handler_t
        >::value
    >::type
    acquired_async_subscribe_imp(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        std::uint8_t qos, Args&&... args) {

        std::vector<std::tuple<as::const_buffer, std::uint8_t>> params;
        params.reserve((sizeof...(args) + 2) / 2);

        std::vector<std::shared_ptr<std::string>> life_keepers;

        async_send_subscribe(
            params,
            life_keepers,
            packet_id,
            topic_name,
            qos,
            std::forward<Args>(args)...
        );
    }

    template <typename... Args>
    typename std::enable_if<
        !std::is_convertible<
            typename std::tuple_element<sizeof...(Args) - 1, std::tuple<Args...>>::type,
            async_handler_t
        >::value
    >::type
    acquired_async_subscribe_imp(
        packet_id_t packet_id,
        std::string const& topic_name,
        std::uint8_t qos, Args&&... args) {

        std::vector<std::tuple<as::const_buffer, std::uint8_t>> params;
        params.reserve((sizeof...(args) + 2) / 2);

        std::vector<std::shared_ptr<std::string>> life_keepers;
        life_keepers.reserve((sizeof...(args) + 2) / 2);

        async_send_subscribe(
            params,
            life_keepers,
            packet_id,
            topic_name,
            qos,
            std::forward<Args>(args)...,
            async_handler_t()
        );
    }

    template <typename... Args>
    typename std::enable_if<
        !std::is_convertible<
            typename std::tuple_element<sizeof...(Args) - 1, std::tuple<Args...>>::type,
            async_handler_t
        >::value
    >::type
    acquired_async_subscribe_imp(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        std::uint8_t qos, Args&&... args) {

        std::vector<std::tuple<as::const_buffer, std::uint8_t>> params;
        params.reserve((sizeof...(args) + 2) / 2);

        std::vector<std::shared_ptr<std::string>> life_keepers;

        async_send_subscribe(
            params,
            life_keepers,
            packet_id,
            topic_name,
            qos,
            std::forward<Args>(args)...,
            async_handler_t()
        );
    }

    template <typename... Args>
    typename std::enable_if<
        std::is_convertible<
            typename std::tuple_element<sizeof...(Args) - 1, std::tuple<Args...>>::type,
            async_handler_t
        >::value
    >::type
    acquired_async_unsubscribe_imp(
        packet_id_t packet_id,
        std::string const& topic_name,
        Args&&... args) {

        std::vector<as::const_buffer> params;
        params.reserve(sizeof...(args));

        std::vector<std::shared_ptr<std::string>> life_keepers;
        life_keepers.reserve(sizeof...(args));

        async_send_unsubscribe(
            params,
            life_keepers,
            packet_id,
            topic_name,
            std::forward<Args>(args)...
        );
    }

    template <typename... Args>
    typename std::enable_if<
        std::is_convertible<
            typename std::tuple_element<sizeof...(Args) - 1, std::tuple<Args...>>::type,
            async_handler_t
        >::value
    >::type
    acquired_async_unsubscribe_imp(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        Args&&... args) {

        std::vector<as::const_buffer> params;
        params.reserve(sizeof...(args));

        std::vector<std::shared_ptr<std::string>> life_keepers;

        async_send_unsubscribe(
            params,
            life_keepers,
            packet_id,
            topic_name,
            std::forward<Args>(args)...
        );
    }

    template <typename... Args>
    typename std::enable_if<
        !std::is_convertible<
            typename std::tuple_element<sizeof...(Args) - 1, std::tuple<Args...>>::type,
            async_handler_t
        >::value
    >::type
    acquired_async_unsubscribe_imp(
        packet_id_t packet_id,
        std::string const& topic_name,
        Args&&... args) {

        std::vector<as::const_buffer> params;
        params.reserve(sizeof...(args) + 1);

        std::vector<std::shared_ptr<std::string>> life_keepers;
        life_keepers.reserve(sizeof...(args) + 1);

        async_send_unsubscribe(
            params,
            life_keepers,
            packet_id,
            topic_name,
            std::forward<Args>(args)...,
            async_handler_t()
        );
    }

    template <typename... Args>
    typename std::enable_if<
        !std::is_convertible<
            typename std::tuple_element<sizeof...(Args) - 1, std::tuple<Args...>>::type,
            async_handler_t
        >::value
    >::type
    acquired_async_unsubscribe_imp(
        packet_id_t packet_id,
        as::const_buffer const& topic_name,
        Args&&... args) {

        std::vector<as::const_buffer> params;
        params.reserve(sizeof...(args) + 1);

        std::vector<std::shared_ptr<std::string>> life_keepers;

        async_send_unsubscribe(
            params,
            life_keepers,
            packet_id,
            topic_name,
            std::forward<Args>(args)...,
            async_handler_t()
        );
    }

    template <typename... Args>
    typename std::enable_if<
        std::is_convertible<
            typename std::tuple_element<sizeof...(Args) - 1, std::tuple<Args...>>::type,
            async_handler_t
        >::value
    >::type
    async_suback_imp(
        packet_id_t packet_id,
        std::uint8_t qos, Args&&... args) {
        std::vector<std::uint8_t> params;
        async_send_suback(params, packet_id, qos, std::forward<Args>(args)...);
    }

    template <typename... Args>
    typename std::enable_if<
        !std::is_convertible<
            typename std::tuple_element<sizeof...(Args) - 1, std::tuple<Args...>>::type,
            async_handler_t
        >::value
    >::type
    async_suback_imp(
        packet_id_t packet_id,
        std::uint8_t qos, Args&&... args) {
        std::vector<std::uint8_t> params;
        async_send_suback(params, packet_id, qos, std::forward<Args>(args)..., async_handler_t());
    }

    class send_buffer {
    public:
        send_buffer():buf_(std::make_shared<std::string>(static_cast<int>(payload_position_), 0)) {}

        std::shared_ptr<std::string> const& buf() const {
            return buf_;
        }

        std::shared_ptr<std::string>& buf() {
            return buf_;
        }

        std::tuple<char*, std::size_t> finalize(std::uint8_t fixed_header) {
            auto rb = remaining_bytes(buf_->size() - payload_position_);
            std::size_t start_position = payload_position_ - rb.size() - 1;
            (*buf_)[start_position] = fixed_header;
            buf_->replace(start_position + 1, rb.size(), rb);
            return std::make_tuple(
                &(*buf_)[start_position],
                buf_->size() - start_position);
        }
    private:
        static constexpr std::size_t const payload_position_ = 5;
        std::shared_ptr<std::string> buf_;
    };

    struct store {
        store(
            packet_id_t id,
            std::uint8_t type,
            basic_store_message_variant<PacketIdBytes> smv,
            life_keeper_t life_keeper)
            :
            packet_id_(id),
            expected_control_packet_type_(type),
            smv_(std::move(smv)),
            life_keeper_(life_keeper) {}
        packet_id_t packet_id() const { return packet_id_; }
        std::uint8_t expected_control_packet_type() const { return expected_control_packet_type_; }
        basic_message_variant<PacketIdBytes> message() const {
            return get_basic_message_variant<PacketIdBytes>(smv_);
        }
    private:
        packet_id_t packet_id_;
        std::uint8_t expected_control_packet_type_;
        basic_store_message_variant<PacketIdBytes> smv_;
        life_keeper_t life_keeper_;
    };

    struct tag_packet_id {};
    struct tag_packet_id_type {};
    struct tag_seq {};
    using mi_store = mi::multi_index_container<
        store,
        mi::indexed_by<
            mi::ordered_unique<
                mi::tag<tag_packet_id_type>,
                mi::composite_key<
                    store,
                    mi::const_mem_fun<
                        store, packet_id_t,
                        &store::packet_id
                    >,
                    mi::const_mem_fun<
                        store, std::uint8_t,
                        &store::expected_control_packet_type
                    >
                >
            >,
            mi::ordered_non_unique<
                mi::tag<tag_packet_id>,
                mi::const_mem_fun<
                    store, packet_id_t,
                    &store::packet_id
                >
            >,
            mi::sequenced<
                mi::tag<tag_seq>
            >
        >
    >;

    void handle_control_packet_type(async_handler_t const& func) {
        fixed_header_ = static_cast<std::uint8_t>(buf_);
        remaining_length_ = 0;
        remaining_length_multiplier_ = 1;
        auto self = this->shared_from_this();
        async_read(
            *socket_,
            as::buffer(&buf_, 1),
            [this, self, func](
                boost::system::error_code const& ec,
                std::size_t bytes_transferred){
                if (handle_close_or_error(ec)) {
                    if (func) func(ec);
                    return;
                }
                if (bytes_transferred != 1) {
                    handle_error(boost::system::errc::make_error_code(boost::system::errc::message_size));
                    if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                    return;
                }
                handle_remaining_length(func);
            }
        );
    }

    void handle_remaining_length(async_handler_t const& func) {
        remaining_length_ += (buf_ & 0b01111111) * remaining_length_multiplier_;
        remaining_length_multiplier_ *= 128;
        if (remaining_length_multiplier_ > 128 * 128 * 128 * 128) {
            handle_error(boost::system::errc::make_error_code(boost::system::errc::message_size));
            if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
            return;
        }
        auto self = this->shared_from_this();
        if (buf_ & 0b10000000) {
            async_read(
                *socket_,
                as::buffer(&buf_, 1),
                [this, self, func](
                    boost::system::error_code const& ec,
                    std::size_t bytes_transferred){
                    if (handle_close_or_error(ec)) {
                        if (func) func(ec);
                        return;
                    }
                    if (bytes_transferred != 1) {
                        handle_error(boost::system::errc::make_error_code(boost::system::errc::message_size));
                        if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                        return;
                    }
                    handle_remaining_length(func);
                }
            );
        }
        else {
            auto check =
                [&]() -> bool {
                    auto cpt = get_control_packet_type(fixed_header_);
                    switch (cpt) {
                    case control_packet_type::connect:
                    case control_packet_type::publish:
                    case control_packet_type::subscribe:
                    case control_packet_type::suback:
                    case control_packet_type::unsubscribe:
                        if (h_is_valid_length_) {
                            return h_is_valid_length_(cpt, remaining_length_);
                        }
                        else {
                            return true;
                        }
                    case control_packet_type::connack:
                        return remaining_length_ == 2;
                    case control_packet_type::puback:
                    case control_packet_type::pubrec:
                    case control_packet_type::pubrel:
                    case control_packet_type::pubcomp:
                    case control_packet_type::unsuback:
                        return remaining_length_ == sizeof(packet_id_t);
                    case control_packet_type::pingreq:
                    case control_packet_type::pingresp:
                    case control_packet_type::disconnect:
                        return remaining_length_ == 0;
                    default:
                        return false;
                    }
                };
            if (!check()) {
                handle_error(boost::system::errc::make_error_code(boost::system::errc::message_size));
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return;
            }
            payload_.resize(remaining_length_);
            if (remaining_length_ == 0) {
                handle_payload(func);
                return;
            }
            async_read(
                *socket_,
                as::buffer(payload_),
                [this, self, func](
                    boost::system::error_code const& ec,
                    std::size_t bytes_transferred){
                    auto g = unique_scope_guard(
                        [this]
                        {
                            payload_.clear();
                        }
                    );
                    if (handle_close_or_error(ec)) {
                        if (func) func(ec);
                        return;
                    }
                    if (bytes_transferred != remaining_length_) {
                        handle_error(boost::system::errc::make_error_code(boost::system::errc::message_size));
                        if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                        return;
                    }
                    handle_payload(func);
                }
            );
        }
    }

    void handle_payload(async_handler_t const& func) {
        auto control_packet_type = get_control_packet_type(fixed_header_);
        bool ret = false;
        switch (control_packet_type) {
        case control_packet_type::connect:
            ret = handle_connect(func);
            break;
        case control_packet_type::connack:
            ret = handle_connack(func);
            break;
        case control_packet_type::publish:
            if (mqtt_connected_) {
                ret = handle_publish(func);
            }
            break;
        case control_packet_type::puback:
            if (mqtt_connected_) {
                ret = handle_puback(func);
            }
            break;
        case control_packet_type::pubrec:
            if (mqtt_connected_) {
                ret = handle_pubrec(func);
            }
            break;
        case control_packet_type::pubrel:
            if (mqtt_connected_) {
                ret = handle_pubrel(func);
            }
            break;
        case control_packet_type::pubcomp:
            if (mqtt_connected_) {
                ret = handle_pubcomp(func);
            }
            break;
        case control_packet_type::subscribe:
            if (mqtt_connected_) {
                ret = handle_subscribe(func);
            }
            break;
        case control_packet_type::suback:
            if (mqtt_connected_) {
                ret = handle_suback(func);
            }
            break;
        case control_packet_type::unsubscribe:
            if (mqtt_connected_) {
                ret = handle_unsubscribe(func);
            }
            break;
        case control_packet_type::unsuback:
            if (mqtt_connected_) {
                ret = handle_unsuback(func);
            }
            break;
        case control_packet_type::pingreq:
            if (mqtt_connected_) {
                ret = handle_pingreq(func);
            }
            break;
        case control_packet_type::pingresp:
            if (mqtt_connected_) {
                ret = handle_pingresp(func);
            }
            break;
        case control_packet_type::disconnect:
            handle_disconnect(func);
            ret = false;
            break;
        default:
            break;
        }
        if (ret) {
            h_mqtt_message_processed_(func);
        }
        else if (func) {
            func(boost::system::errc::make_error_code(boost::system::errc::success));
        }
    }

    void handle_close() {
        if (h_close_) h_close_();
    }

    void handle_error(boost::system::error_code const& ec) {
        if (h_error_) h_error_(ec);
    }

    bool handle_connect(async_handler_t const& func) {
        std::size_t i = 0;
        if (remaining_length_ < 10 || // *1
            payload_[i++] != 0x00 ||
            payload_[i++] != 0x04 ||
            payload_[i++] != 'M' ||
            payload_[i++] != 'Q' ||
            payload_[i++] != 'T' ||
            payload_[i++] != 'T') {
            if (func) func(boost::system::errc::make_error_code(boost::system::errc::protocol_error));
            return false;
        }

        auto version = static_cast<std::uint8_t>(payload_[i++]);
        if (version != protocol_version::v3_1_1 && version != protocol_version::v5) {
            if (func) {
                func(boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported));
            }
            return false;
        }

        if (version_ == protocol_version::undetermined) {
            version_ = version;
        }
        else if (version_ != version) {
            if (func) {
                func(boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported));
            }
            return false;
        }

        char byte8 = payload_[i++];

        std::uint16_t keep_alive;
        keep_alive = make_uint16_t(payload_[i], payload_[i + 1]); // index is checked at *1
        i += 2;

        std::vector<v5::property_variant> props;
        if (version_ == protocol_version::v5) {
            char const* b = &payload_[i];
            char const* it = b;
            char const* e = b + payload_.size();
            if (auto props_opt = v5::property::parse_with_length(it, e)) {
                props = std::move(*props_opt);
            }
            else {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
            i += std::distance(b, it);
        }

        if (remaining_length_ < i + 2) {
            if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
            return false;
        }
        std::uint16_t client_id_length;
        client_id_length = make_uint16_t(payload_[i], payload_[i + 1]);
        i += 2;

        if (remaining_length_ < i + client_id_length) {
            if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
            return false;
        }
        std::string client_id(payload_.data() + i, client_id_length);
        if (utf8string::validate_contents(client_id) != utf8string::validation::well_formed) {
            if (func) func(boost::system::errc::make_error_code(boost::system::errc::bad_message));
            return false;
        }
        client_id_ = std::move(client_id);
        i += client_id_length;

        clean_session_ = connect_flags::has_clean_session(byte8);
        boost::optional<will> w;
        if (connect_flags::has_will_flag(byte8)) {

            if (remaining_length_ < i + 2) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
            std::uint16_t topic_name_length;
            topic_name_length = make_uint16_t(payload_[i], payload_[i + 1]);
            i += 2;

            if (remaining_length_ < i + topic_name_length) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
            std::string topic_name(payload_.data() + i, topic_name_length);
            if (utf8string::validate_contents(topic_name) != utf8string::validation::well_formed) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::bad_message));
                return false;
            }

            i += topic_name_length;

            if (remaining_length_ < i + 2) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
            std::uint16_t will_message_length;
            will_message_length = make_uint16_t(payload_[i], payload_[i + 1]);
            i += 2;

            if (remaining_length_ < i + will_message_length) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
            std::string will_message(payload_.data() + i, will_message_length);
            i += will_message_length;
            w = will(topic_name,
                     will_message,
                     connect_flags::has_will_retain(byte8),
                     connect_flags::will_qos(byte8));
        }

        boost::optional<std::string> user_name;
        if (connect_flags::has_user_name_flag(byte8)) {

            if (remaining_length_ < i + 2) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
            std::uint16_t user_name_length;
            user_name_length = make_uint16_t(payload_[i], payload_[i + 1]);
            i += 2;

            if (remaining_length_ < i + user_name_length) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
            user_name = std::string(payload_.data() + i, user_name_length);
            if (utf8string::validate_contents(user_name.get()) != utf8string::validation::well_formed) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::bad_message));
                return false;
            }
            i += user_name_length;
        }

        boost::optional<std::string> password;
        if (connect_flags::has_password_flag(byte8)) {

            if (remaining_length_ < i + 2) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
            std::uint16_t password_length;
            password_length = make_uint16_t(payload_[i], payload_[i + 1]);
            i += 2;

            if (remaining_length_ < i + password_length) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
            password = std::string(payload_.data() + i, password_length);
            i += password_length;
        }
        mqtt_connected_ = true;

        if (clean_session_) {
            LockGuard<Mutex> lck (store_mtx_);
            store_.clear();
            packet_id_.clear();
        }

        switch (version_ ) {
        case protocol_version::v3_1_1:
            if (h_connect_) {
                if (h_connect_(
                        client_id_,
                        user_name,
                        password,
                        std::move(w),
                        clean_session_,
                        keep_alive)
                ) {
                    return true;
                }
                return false;
            }
            break;
        case protocol_version::v5:
            if (h_v5_connect_) {
                if (h_v5_connect_(
                        client_id_,
                        user_name,
                        password,
                        std::move(w),
                        clean_session_,
                        keep_alive,
                        props)
                ) {
                    return true;
                }
                return false;
            }
            break;
        default:
            BOOST_ASSERT(false);
            return false;
        }
        return true;
    }

    bool handle_connack(async_handler_t const& func) {
        if (!connect_requested_) {
            if (func) func(boost::system::errc::make_error_code(boost::system::errc::protocol_error));
            return false;
        }
        connect_requested_ = false;
        if (static_cast<std::uint8_t>(payload_[1]) == connect_return_code::accepted) {
            if (clean_session_) {
                LockGuard<Mutex> lck (store_mtx_);
                store_.clear();
                packet_id_.clear();
            }
            else {
                LockGuard<Mutex> lck (store_mtx_);
                auto& idx = store_.template get<tag_seq>();
                for (auto const& e : idx) {
                    do_sync_write(e.message());
                }
            }
        }
        bool session_present = is_session_present(payload_[0]);

        std::vector<v5::property_variant> props;
        if (version_ == protocol_version::v5) {
            char const* b = &payload_[2];
            char const* it = b;
            char const* e = b + payload_.size();
            if (auto props_opt = v5::property::parse_with_length(it, e)) {
                props = std::move(*props_opt);
            }
            else {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
        }

        mqtt_connected_ = true;

        switch (version_ ) {
        case protocol_version::v3_1_1:
            if (h_connack_) {
                return
                    h_connack_(
                        session_present,
                        static_cast<std::uint8_t>(payload_[1])
                    );
            }
            break;
        case protocol_version::v5:
            if (h_v5_connack_) {
                return
                    h_v5_connack_(
                        session_present,
                        static_cast<std::uint8_t>(payload_[1]),
                        std::move(props)
                    );
            }
            break;
        default:
            BOOST_ASSERT(false);
            return false;
        }

        return true;
    }

    template <typename F, typename AF>
    void auto_pub_response(F const& f, AF const& af) {
        if (auto_pub_response_) {
            if (auto_pub_response_async_) af();
            else f();
        }
    }

    bool handle_publish(async_handler_t const& func) {
        if (remaining_length_ < 2) {
            if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
            return false;
        }
        std::size_t i = 0;
        std::uint16_t topic_name_length = make_uint16_t(payload_[i], payload_[i + 1]);
        i += 2;

        if (remaining_length_ < i + topic_name_length) {
            if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
            return false;
        }
        std::string topic_name(payload_.data() + i, topic_name_length);
        if (utf8string::validate_contents(topic_name) != utf8string::validation::well_formed) {
            if (func) func(boost::system::errc::make_error_code(boost::system::errc::bad_message));
            return false;
        }
        i += topic_name_length;

        boost::optional<packet_id_t> packet_id;
        auto qos = publish::get_qos(fixed_header_);
        switch (qos) {
        case qos::at_most_once:
            if (h_publish_) {
                std::string contents(payload_.data() + i, payload_.size() - i);
                return h_publish_(fixed_header_, packet_id, std::move(topic_name), std::move(contents));
            }
            break;
        case qos::at_least_once: {
            if (remaining_length_ < i + sizeof(packet_id_t)) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
            packet_id = make_packet_id<PacketIdBytes>::apply(
                &payload_[i],
                &payload_[i + sizeof(packet_id_t)]
            );
            i += sizeof(packet_id_t);
            auto res = [this, &packet_id, &func] {
                auto_pub_response(
                    [this, &packet_id] {
                        if (connected_) send_puback(*packet_id);
                    },
                    [this, &packet_id, &func] {
                        if (connected_) async_send_puback(*packet_id, func);
                    }
                );
            };
            if (h_publish_) {
                std::string contents(payload_.data() + i, payload_.size() - i);
                if (h_publish_(fixed_header_, packet_id, std::move(topic_name), std::move(contents))) {
                    res();
                    return true;
                }
                return false;
            }
            res();
        } break;
        case qos::exactly_once: {
            if (remaining_length_ < i + sizeof(packet_id_t)) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
            packet_id = make_packet_id<PacketIdBytes>::apply(
                &payload_[i],
                &payload_[i + sizeof(packet_id_t)]
            );
            i += sizeof(packet_id_t);
            auto res = [this, &packet_id, &func] {
                auto_pub_response(
                    [this, &packet_id] {
                        if (connected_) send_pubrec(*packet_id);
                    },
                    [this, &packet_id, &func] {
                        if (connected_) async_send_pubrec(*packet_id, func);
                    }
                );
            };
            if (h_publish_) {
                auto it = qos2_publish_handled_.find(*packet_id);
                if (it == qos2_publish_handled_.end()) {
                    std::string contents(payload_.data() + i, payload_.size() - i);
                    if (h_publish_(fixed_header_, packet_id, std::move(topic_name), std::move(contents))) {
                        qos2_publish_handled_.emplace(*packet_id);
                        res();
                        return true;
                    }
                    return false;
                }
            }
            res();
        } break;
        default:
            break;
        }
        return true;
    }

    bool handle_puback(async_handler_t const& /*func*/) {
        packet_id_t packet_id = make_packet_id<PacketIdBytes>::apply(
            &payload_[0],
            &payload_[0 + sizeof(packet_id_t)]
        );
        {
            LockGuard<Mutex> lck (store_mtx_);
            auto& idx = store_.template get<tag_packet_id_type>();
            auto r = idx.equal_range(std::make_tuple(packet_id, control_packet_type::puback));
            idx.erase(std::get<0>(r), std::get<1>(r));
            packet_id_.erase(packet_id);
        }
        if (h_serialize_remove_) h_serialize_remove_(packet_id);
        if (h_puback_) return h_puback_(packet_id);
        return true;
    }

    bool handle_pubrec(async_handler_t const& func) {
        packet_id_t packet_id = make_packet_id<PacketIdBytes>::apply(
            &payload_[0],
            &payload_[0 + sizeof(packet_id_t)]
        );
        {
            LockGuard<Mutex> lck (store_mtx_);
            auto& idx = store_.template get<tag_packet_id_type>();
            auto r = idx.equal_range(std::make_tuple(packet_id, control_packet_type::pubrec));
            idx.erase(std::get<0>(r), std::get<1>(r));
            // packet_id shouldn't be erased here.
            // It is reused for pubrel/pubcomp.
        }
        auto res = [this, &packet_id, &func] {
            auto_pub_response(
                [this, &packet_id] {
                    if (connected_) send_pubrel(packet_id);
                    else store_pubrel(packet_id);
                },
                [this, &packet_id, &func] {
                    if (connected_) async_send_pubrel(packet_id, func);
                    else store_pubrel(packet_id);
                }
            );
        };
        if (h_pubrec_) {
            if (h_pubrec_(packet_id)) {
                res();
                return true;
            }
            return false;
        }
        res();
        return true;
    }

    bool handle_pubrel(async_handler_t const& func) {
        packet_id_t packet_id = make_packet_id<PacketIdBytes>::apply(
            &payload_[0],
            &payload_[0 + sizeof(packet_id_t)]
        );
        auto res = [this, &packet_id, &func] {
            auto_pub_response(
                [this, &packet_id] {
                    if (connected_) send_pubcomp(packet_id);
                },
                [this, &packet_id, &func] {
                    if (connected_) async_send_pubcomp(packet_id, func);
                }
            );
        };
        qos2_publish_handled_.erase(packet_id);
        if (h_pubrel_) {
            if (h_pubrel_(packet_id)) {
                res();
                return true;
            }
            return false;
        }
        res();
        return true;
    }

    bool handle_pubcomp(async_handler_t const& /*func*/) {
        packet_id_t packet_id = make_packet_id<PacketIdBytes>::apply(
            &payload_[0],
            &payload_[0 + sizeof(packet_id_t)]
        );
        {
            LockGuard<Mutex> lck (store_mtx_);
            auto& idx = store_.template get<tag_packet_id_type>();
            auto r = idx.equal_range(std::make_tuple(packet_id, control_packet_type::pubcomp));
            idx.erase(std::get<0>(r), std::get<1>(r));
            packet_id_.erase(packet_id);
        }
        if (h_serialize_remove_) h_serialize_remove_(packet_id);
        if (h_pubcomp_) return h_pubcomp_(packet_id);
        return true;
    }

    bool handle_subscribe(async_handler_t const& func) {
        std::size_t i = 0;
        if (remaining_length_ < sizeof(packet_id_t)) {
            if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
            return false;
        }
        packet_id_t packet_id = make_packet_id<PacketIdBytes>::apply(
            &payload_[0],
            &payload_[0 + sizeof(packet_id_t)]
        );
        i += sizeof(packet_id_t);
        std::vector<std::tuple<std::string, std::uint8_t>> entries;
        while (i < remaining_length_) {
            if (remaining_length_ < i + 2) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
            std::uint16_t topic_length = make_uint16_t(payload_[i], payload_[i + 1]);
            i += 2;

            if (remaining_length_ < i + topic_length) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
            std::string topic_filter(payload_.data() + i, topic_length);
            if (utf8string::validate_contents(topic_filter) != utf8string::validation::well_formed) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::bad_message));
                return false;
            }
            i += topic_length;

            if (remaining_length_ < i + 1) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
            std::uint8_t qos = payload_[i] & 0b00000011;
            ++i;

            entries.emplace_back(std::move(topic_filter), qos);
        }
        if (h_subscribe_) return h_subscribe_(packet_id, std::move(entries));
        return true;
    }

    bool handle_suback(async_handler_t const& func) {
        if (remaining_length_ < sizeof(packet_id_t)) {
            if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
            return false;
        }
        packet_id_t packet_id = make_packet_id<PacketIdBytes>::apply(
            &payload_[0],
            &payload_[0 + sizeof(packet_id_t)]
        );
        {
            LockGuard<Mutex> lck (store_mtx_);
            packet_id_.erase(packet_id);
        }
        std::vector<boost::optional<std::uint8_t>> results;
        results.reserve(payload_.size() - sizeof(packet_id_t));
        auto it = payload_.cbegin() + sizeof(packet_id_t);
        auto end = payload_.cend();
        for (; it != end; ++it) {
            if (*it & 0b10000000) {
                results.push_back(boost::none);
            }
            else {
                results.push_back(*it);
            }
        }
        if (h_suback_) return h_suback_(packet_id, std::move(results));
        return true;
    }

    bool handle_unsubscribe(async_handler_t const& func) {
        std::size_t i = 0;
        if (remaining_length_ < sizeof(packet_id_t)) {
            if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
            return false;
        }
        packet_id_t packet_id = make_packet_id<PacketIdBytes>::apply(
            &payload_[0],
            &payload_[0 + sizeof(packet_id_t)]
        );
        i += sizeof(packet_id_t);
        std::vector<std::string> topic_filters;
        while (i < remaining_length_) {
            if (remaining_length_ < i + 2) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
            std::uint16_t topic_length = make_uint16_t(payload_[i], payload_[i + 1]);
            i += 2;
            if (remaining_length_ < i + topic_length) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::message_size));
                return false;
            }
            std::string topic_filter(payload_.data() + i, topic_length);
            if (utf8string::validate_contents(topic_filter) != utf8string::validation::well_formed) {
                if (func) func(boost::system::errc::make_error_code(boost::system::errc::bad_message));
                return false;
            }
            i += topic_length;

            topic_filters.emplace_back(std::move(topic_filter));
        }
        if (h_unsubscribe_) return h_unsubscribe_(packet_id, std::move(topic_filters));
        return true;
    }

    bool handle_unsuback(async_handler_t const& /*func*/) {
        packet_id_t packet_id = make_packet_id<PacketIdBytes>::apply(
            &payload_[0],
            &payload_[0 + sizeof(packet_id_t)]
        );
        {
            LockGuard<Mutex> lck (store_mtx_);
            packet_id_.erase(packet_id);
        }
        if (h_unsuback_) return h_unsuback_(packet_id);
        return true;
    }

    bool handle_pingreq(async_handler_t const& /*func*/) {
        if (h_pingreq_) return h_pingreq_();
        return true;
    }

    bool handle_pingresp(async_handler_t const& /*func*/) {
        if (h_pingresp_) return h_pingresp_();
        return true;
    }

    void handle_disconnect(async_handler_t const& /*func*/) {
        if (h_disconnect_) h_disconnect_();
    }

    // Blocking senders.
    void send_connect(std::uint16_t keep_alive_sec) {
        do_sync_write(
            connect_message(
                keep_alive_sec,
                client_id_,
                clean_session_,
                will_,
                user_name_,
                password_
            )
        );
    }

    void send_connack(bool session_present, std::uint8_t return_code) {
        do_sync_write(
            connack_message(
                session_present,
                return_code
            )
        );
    }

    void send_publish(
        as::const_buffer const& topic_name,
        std::uint8_t qos,
        bool retain,
        bool dup,
        packet_id_t packet_id,
        boost::optional<std::vector<v5::property_variant>> props,
        as::const_buffer const& payload,
        life_keeper_t life_keeper) {

        auto g = shared_scope_guard(
            [MQTT_CAPTURE_MOVE(life_keeper)] {
                if (life_keeper) life_keeper();
            }
        );

        auto msg =
            basic_publish_message<PacketIdBytes>(
                topic_name,
                qos,
                retain,
                dup,
                packet_id,
                payload
            );

        auto do_send_publish =
            [&](auto msg, auto const& serialize_publish) {

                if (qos == qos::at_least_once || qos == qos::exactly_once) {
                    auto store_msg = msg;
                    store_msg.set_dup(true);
                    LockGuard<Mutex> lck (store_mtx_);
                    store_.emplace(
                        packet_id,
                        qos == qos::at_least_once ? control_packet_type::puback
                        : control_packet_type::pubrec,
                        store_msg,
                        [g] {}
                    );
                    if (serialize_publish) {
                        serialize_publish(store_msg);
                    }
                }
                do_sync_write(msg);
            };
        if (props) {
            do_send_publish(
                v5::basic_publish_message<PacketIdBytes>(
                    topic_name,
                    qos,
                    retain,
                    dup,
                    packet_id,
                    std::move(props.get()),
                    payload
                ),
                h_serialize_v5_publish_
            );
        }
        else {
            do_send_publish(
                v3_1_1::basic_publish_message<PacketIdBytes>(
                    topic_name,
                    qos,
                    retain,
                    dup,
                    packet_id,
                    payload
                ),
                h_serialize_publish_
            );
        }
    }

    void send_puback(packet_id_t packet_id) {
        do_sync_write(basic_puback_message<PacketIdBytes>(packet_id));
        if (h_pub_res_sent_) h_pub_res_sent_(packet_id);
    }

    void send_pubrec(packet_id_t packet_id) {
        do_sync_write(basic_pubrec_message<PacketIdBytes>(packet_id));
    }

    void send_pubrel(packet_id_t packet_id) {

        auto msg = basic_pubrel_message<PacketIdBytes>(packet_id);

        {
            LockGuard<Mutex> lck (store_mtx_);

            // insert if not registerd (start from pubrel sending case)
            packet_id_.insert(packet_id);

            auto ret = store_.emplace(
                packet_id,
                control_packet_type::pubcomp,
                msg,
                [] {}
            );
            BOOST_ASSERT(ret.second);
        }

        if (h_serialize_pubrel_) {
            h_serialize_pubrel_(msg);
        }

        do_sync_write(msg);

    }

    void store_pubrel(packet_id_t packet_id) {

        auto msg = basic_pubrel_message<PacketIdBytes>(packet_id);

        {
            LockGuard<Mutex> lck (store_mtx_);
            auto ret = store_.emplace(
                packet_id,
                control_packet_type::pubcomp,
                msg,
                [] {}
            );
            BOOST_ASSERT(ret.second);
            if (h_serialize_pubrel_) {
                h_serialize_pubrel_(msg);
            }
        }
    }

    void send_pubcomp(packet_id_t packet_id) {
        do_sync_write(basic_pubcomp_message<PacketIdBytes>(packet_id));
        if (h_pub_res_sent_) h_pub_res_sent_(packet_id);
    }

    template <typename... Args>
    void send_subscribe(
        std::vector<std::tuple<as::const_buffer, std::uint8_t>>& params,
        packet_id_t packet_id,
        as::const_buffer topic_name,
        std::uint8_t qos, Args... args) {
        params.emplace_back(std::move(topic_name), qos);
        send_subscribe(params, packet_id, args...);
    }

    template <typename... Args>
    void send_subscribe(
        std::vector<std::tuple<as::const_buffer, std::uint8_t>>& params,
        packet_id_t packet_id,
        std::string const& topic_name,
        std::uint8_t qos, Args... args) {
        params.emplace_back(as::buffer(topic_name), qos);
        send_subscribe(params, packet_id, args...);
    }

    void send_subscribe(
        std::vector<std::tuple<as::const_buffer, std::uint8_t>> const& params,
        packet_id_t packet_id) {
        do_sync_write(basic_subscribe_message<PacketIdBytes>(params, packet_id));
    }

    template <typename... Args>
    void send_suback(
        std::vector<std::uint8_t>& params,
        packet_id_t packet_id,
        std::uint8_t qos, Args&&... args) {
        params.push_back(qos);
        send_suback(params, packet_id, std::forward<Args>(args)...);
    }

    void send_suback(
        std::vector<std::uint8_t> const& params,
        packet_id_t packet_id) {
        do_sync_write(basic_suback_message<PacketIdBytes>(params, packet_id));
    }

    template <typename... Args>
    void send_unsubscribe(
        std::vector<as::const_buffer>& params,
        packet_id_t packet_id,
        as::const_buffer topic_name,
        Args... args) {
        params.emplace_back(std::move(topic_name));
        send_unsubscribe(params, packet_id, args...);
    }

    template <typename... Args>
    void send_unsubscribe(
        std::vector<as::const_buffer>& params,
        packet_id_t packet_id,
        std::string const&  topic_name,
        Args... args) {
        params.emplace_back(as::buffer(topic_name));
        send_unsubscribe(params, packet_id, args...);
    }

    void send_unsubscribe(
        std::vector<as::const_buffer> const& params,
        packet_id_t packet_id) {
        do_sync_write(basic_unsubscribe_message<PacketIdBytes>(params, packet_id));
    }

    void send_unsuback(
        packet_id_t packet_id) {
        do_sync_write(basic_unsuback_message<PacketIdBytes>(packet_id));
    }

    void send_pingreq() {
        do_sync_write(pingreq_message());
    }

    void send_pingresp() {
        do_sync_write(pingresp_message());
    }

    void send_disconnect() {
        do_sync_write(disconnect_message());
    }

    // Blocking write
    template <typename MessageVariant>
    void do_sync_write(MessageVariant&& mv) {
        boost::system::error_code ec;
        if (!connected_) return;
        if (h_pre_send_) h_pre_send_();
        write(*socket_, const_buffer_sequence<PacketIdBytes>(std::forward<MessageVariant>(mv)), ec);
        if (ec) handle_error(ec);
    }

    // Non blocking (async) senders
    void async_send_connect(std::uint16_t keep_alive_sec, async_handler_t const& func) {
        do_async_write(
            connect_message(
                keep_alive_sec,
                client_id_,
                clean_session_,
                will_,
                user_name_,
                password_
            ),
            func
        );
    }

    void async_send_connack(bool session_present, std::uint8_t return_code, async_handler_t const& func) {
        do_async_write(
            connack_message(
                session_present,
                return_code
            ),
            func
        );
    }

    void async_send_publish(
        as::const_buffer const& topic_name,
        std::uint8_t qos,
        bool retain,
        bool dup,
        packet_id_t packet_id,
        boost::optional<std::vector<v5::property_variant>> props,
        as::const_buffer const& payload,
        async_handler_t const& func,
        life_keeper_t life_keeper) {

        auto g = shared_scope_guard(
            [MQTT_CAPTURE_MOVE(life_keeper)] {
                if (life_keeper) life_keeper();
            }
        );

        auto do_async_send_publish =
            [&](auto msg, auto const& serialize_publish) {
                auto store_msg = msg;
                store_msg.set_dup(true);
                {
                    LockGuard<Mutex> lck (store_mtx_);
                    auto ret = store_.emplace(
                        packet_id,
                        qos == qos::at_least_once ? control_packet_type::puback
                                                  : control_packet_type::pubrec,
                        store_msg,
                        [g] {}
                    );
                    BOOST_ASSERT(ret.second);
                }

                if (serialize_publish) {
                    serialize_publish(store_msg);
                }

                do_async_write(
                    msg,
                    [g, func](boost::system::error_code const& ec) {
                        if (func) func(ec);
                    }
                );
            };

        if (props) {
            do_async_send_publish(
                v5::basic_publish_message<PacketIdBytes>(
                    topic_name,
                    qos,
                    retain,
                    dup,
                    packet_id,
                    std::move(props.get()),
                    payload
                ),
                h_serialize_v5_publish_
            );
        }
        else {
            do_async_send_publish(
                v3_1_1::basic_publish_message<PacketIdBytes>(
                    topic_name,
                    qos,
                    retain,
                    dup,
                    packet_id,
                    payload
                ),
                h_serialize_publish_
            );
        }
    }

    void async_send_puback(packet_id_t packet_id, async_handler_t const& func) {
        auto self = this->shared_from_this();
        do_async_write(
            basic_puback_message<PacketIdBytes>(packet_id),
            [this, self, packet_id, func]
            (boost::system::error_code const& ec){
                if (func) func(ec);
                if (h_pub_res_sent_) h_pub_res_sent_(packet_id);
            }
        );
    }

    void async_send_pubrec(packet_id_t packet_id, async_handler_t const& func) {
        do_async_write(
            basic_pubrec_message<PacketIdBytes>(packet_id),
            func
        );
    }

    void async_send_pubrel(packet_id_t packet_id, async_handler_t const& func) {

        auto msg = basic_pubrel_message<PacketIdBytes>(packet_id);

        {
            LockGuard<Mutex> lck (store_mtx_);

            // insert if not registerd (start from pubrel sending case)
            packet_id_.insert(packet_id);

            auto ret = store_.emplace(
                packet_id,
                control_packet_type::pubcomp,
                msg,
                [] {});
            BOOST_ASSERT(ret.second);
        }

        if (h_serialize_pubrel_) {
            h_serialize_pubrel_(msg);
        }
        do_async_write(msg, func);
    }

    void async_send_pubcomp(packet_id_t packet_id, async_handler_t const& func) {
        auto self = this->shared_from_this();
        do_async_write(
            basic_pubcomp_message<PacketIdBytes>(packet_id),
            [this, self, packet_id, func]
            (boost::system::error_code const& ec){
                if (func) func(ec);
                if (h_pub_res_sent_) h_pub_res_sent_(packet_id);
            }
        );
    }

    template <typename... Args>
    void async_send_subscribe(
        std::vector<std::tuple<as::const_buffer, std::uint8_t>>& params,
        std::vector<std::shared_ptr<std::string>>& life_keepers,
        packet_id_t packet_id,
        as::const_buffer topic_name,
        std::uint8_t qos,
        Args... args) {

        params.emplace_back(std::move(topic_name), qos);
        async_send_subscribe(params, life_keepers, packet_id, args...);
    }

    template <typename... Args>
    void async_send_subscribe(
        std::vector<std::tuple<as::const_buffer, std::uint8_t>>& params,
        std::vector<std::shared_ptr<std::string>>& life_keepers,
        packet_id_t packet_id,
        std::string const& topic_name,
        std::uint8_t qos,
        Args... args) {

        life_keepers.emplace_back(std::make_shared<std::string>(topic_name));
        params.emplace_back(as::buffer(*life_keepers.back()), qos);
        async_send_subscribe(params, life_keepers, packet_id, args...);
    }

    void async_send_subscribe(
        std::vector<std::tuple<as::const_buffer, std::uint8_t>> const& params,
        std::vector<std::shared_ptr<std::string>> const& life_keepers,
        packet_id_t packet_id,
        async_handler_t const& func) {

        do_async_write(
            basic_subscribe_message<PacketIdBytes>(params, packet_id),
            [life_keepers, func]
            (boost::system::error_code const& ec) {
                if (func) func(ec);
            }
        );
    }

    template <typename... Args>
    void async_send_suback(
        std::vector<std::uint8_t>& params,
        packet_id_t packet_id,
        std::uint8_t qos, Args... args) {
        params.push_back(qos);
        async_send_suback(params, packet_id, args...);
    }

    void async_send_suback(
        std::vector<std::uint8_t> const& params,
        packet_id_t packet_id,
        async_handler_t const& func) {
        do_async_write(basic_suback_message<PacketIdBytes>(params, packet_id), func);
    }

    template <typename... Args>
    void async_send_unsubscribe(
        std::vector<as::const_buffer>& params,
        std::vector<std::shared_ptr<std::string>>& life_keepers,
        packet_id_t packet_id,
        as::const_buffer topic_name,
        Args... args) {
        params.emplace_back(std::move(topic_name));
        async_send_unsubscribe(params, life_keepers, packet_id, args...);
    }

    template <typename... Args>
    void async_send_unsubscribe(
        std::vector<as::const_buffer>& params,
        std::vector<std::shared_ptr<std::string>>& life_keepers,
        packet_id_t packet_id,
        std::string const& topic_name,
        Args... args) {

        life_keepers.emplace_back(std::make_shared<std::string>(topic_name));
        params.emplace_back(as::buffer(*life_keepers.back()));
        async_send_unsubscribe(params, life_keepers, packet_id, args...);
    }

    void async_send_unsubscribe(
        std::vector<as::const_buffer> const& params,
        std::vector<std::shared_ptr<std::string>> const& life_keepers,
        packet_id_t packet_id,
        async_handler_t const& func) {
        do_async_write(
            basic_unsubscribe_message<PacketIdBytes>(
                params,
                packet_id
            ),
            [life_keepers, func]
            (boost::system::error_code const& ec) {
                if (func) func(ec);
            }
        );
    }

    void async_send_unsuback(
        packet_id_t packet_id, async_handler_t const& func) {
        do_async_write(basic_unsuback_message<PacketIdBytes>(packet_id), func);
    }

    void async_send_pingreq(async_handler_t const& func) {
        do_async_write(pingreq_message(), func);
    }

    void async_send_pingresp(async_handler_t const& func) {
        do_async_write(pingresp_message(), func);
    }

    void async_send_disconnect(async_handler_t const& func) {
        do_async_write(disconnect_message(), func);
    }

    // Non blocking (async) write

    class async_packet {
    public:
        async_packet(
            basic_message_variant<PacketIdBytes> const& mv,
            async_handler_t h = async_handler_t())
            :
            mv_(mv), handler_(std::move(h)) {}
        async_packet(
            basic_message_variant<PacketIdBytes>&& mv,
            async_handler_t h = async_handler_t())
            :
            mv_(std::move(mv)), handler_(std::move(h)) {}
        basic_message_variant<PacketIdBytes> const& message() const {
            return mv_;
        }
        basic_message_variant<PacketIdBytes>& message() {
            return mv_;
        }
        async_handler_t const& handler() const { return handler_; }
        async_handler_t& handler() { return handler_; }
    private:
        basic_message_variant<PacketIdBytes> mv_;
        async_handler_t handler_;
    };

    void do_async_write(basic_message_variant<PacketIdBytes> mv, async_handler_t const& func) {
        auto self = this->shared_from_this();
        socket_->post(
            [this, self, MQTT_CAPTURE_MOVE(mv), func]
            () {
                if (!connected_) {
                    // offline async publish is successfully finished
                    if (func) func(boost::system::errc::make_error_code(boost::system::errc::success));
                    return;
                }
                queue_.emplace_back(std::move(mv), func);
                if (queue_.size() > 1) return;
                do_async_write();
            }
        );
    }

    void do_async_write() {
        auto const& elem = queue_.front();
        auto const& mv = elem.message();
        auto const& func = elem.handler();
        auto self = this->shared_from_this();
        if (h_pre_send_) h_pre_send_();
        async_write(
            *socket_,
            const_buffer_sequence(mv),
            write_completion_handler(
                this->shared_from_this(),
                func,
                mqtt::size<PacketIdBytes>(mv)
            )
        );
    }

    static std::uint16_t make_uint16_t(char b1, char b2) {
        return
            ((static_cast<std::uint16_t>(b1) & 0xff)) << 8 |
            (static_cast<std::uint16_t>(b2) & 0xff);
    }

    struct write_completion_handler {
        write_completion_handler(
            std::shared_ptr<this_type> const& self,
            async_handler_t const& func,
            std::size_t expected)
            :self_(self),
             func_(func),
             expected_(expected)
        {}
        void operator()(boost::system::error_code const& ec) const {
            if (func_) func_(ec);
            self_->queue_.pop_front();
            if (ec || // Error is handled by async_read.
                !self_->connected_) {
                self_->connected_ = false;
                while (!self_->queue_.empty()) {
                    self_->queue_.front().handler()(ec);
                    self_->queue_.pop_front();
                }
                return;
            }
            if (!self_->queue_.empty()) {
                self_->do_async_write();
            }
        }
        void operator()(
            boost::system::error_code const& ec,
            std::size_t bytes_transferred) const {
            if (func_) func_(ec);
            self_->queue_.pop_front();
            if (ec || // Error is handled by async_read.
                !self_->connected_) {
                self_->connected_ = false;
                while (!self_->queue_.empty()) {
                    self_->queue_.front().handler()(ec);
                    self_->queue_.pop_front();
                }
                return;
            }
            if (expected_ != bytes_transferred) {
                self_->connected_ = false;
                while (!self_->queue_.empty()) {
                    self_->queue_.front().handler()(ec);
                    self_->queue_.pop_front();
                }
                throw write_bytes_transferred_error(expected_, bytes_transferred);
            }
            if (!self_->queue_.empty()) {
                self_->do_async_write();
            }
        }
        std::shared_ptr<this_type> self_;
        async_handler_t func_;
        std::size_t expected_;
    };

private:
    std::unique_ptr<Socket> socket_;
    std::string host_;
    std::string port_;
    std::atomic<bool> connected_;
    std::atomic<bool> mqtt_connected_;
    std::string client_id_;
    bool clean_session_;
    boost::optional<will> will_;
    char buf_;
    std::uint8_t fixed_header_;
    std::size_t remaining_length_multiplier_;
    std::size_t remaining_length_;
    std::vector<char> payload_;

    // MQTT common handlers
    pingreq_handler h_pingreq_;
    pingresp_handler h_pingresp_;

    // MQTT v3_1_1 handlers
    connect_handler h_connect_;
    connack_handler h_connack_;
    publish_handler h_publish_;
    puback_handler h_puback_;
    pubrec_handler h_pubrec_;
    pubrel_handler h_pubrel_;
    pubcomp_handler h_pubcomp_;
    subscribe_handler h_subscribe_;
    suback_handler h_suback_;
    unsubscribe_handler h_unsubscribe_;
    unsuback_handler h_unsuback_;
    disconnect_handler h_disconnect_;

    // MQTT v5 handlers
    v5_connect_handler h_v5_connect_;
    v5_connack_handler h_v5_connack_;
    v5_publish_handler h_v5_publish_;
    v5_puback_handler h_v5_puback_;
    v5_pubrec_handler h_v5_pubrec_;
    v5_pubrel_handler h_v5_pubrel_;
    v5_pubcomp_handler h_v5_pubcomp_;
    v5_subscribe_handler h_v5_subscribe_;
    v5_suback_handler h_v5_suback_;
    v5_unsubscribe_handler h_v5_unsubscribe_;
    v5_unsuback_handler h_v5_unsuback_;
    v5_disconnect_handler h_v5_disconnect_;

    // original handlers
    close_handler h_close_;
    error_handler h_error_;
    pub_res_sent_handler h_pub_res_sent_;
    serialize_publish_message_handler h_serialize_publish_;
    serialize_v5_publish_message_handler h_serialize_v5_publish_;
    serialize_pubrel_message_handler h_serialize_pubrel_;
    serialize_v5_pubrel_message_handler h_serialize_v5_pubrel_;
    serialize_remove_handler h_serialize_remove_;
    pre_send_handler h_pre_send_;
    is_valid_length_handler h_is_valid_length_;

    boost::optional<std::string> user_name_;
    boost::optional<std::string> password_;
    Mutex store_mtx_;
    mi_store store_;
    std::set<packet_id_t> qos2_publish_handled_;
    std::deque<async_packet> queue_;
    packet_id_t packet_id_master_;
    std::set<packet_id_t> packet_id_;
    bool auto_pub_response_;
    bool auto_pub_response_async_;
    bool disconnect_requested_;
    bool connect_requested_;
    mqtt_message_processed_handler h_mqtt_message_processed_;
    std::uint8_t version_;
};

} // namespace mqtt

#endif // MQTT_ENDPOINT_HPP
