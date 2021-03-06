// Copyright Takatoshi Kondo 2018
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#include "test_main.hpp"
#include "combi_test.hpp"

BOOST_AUTO_TEST_SUITE(test_resend_serialize)

template <typename Client, typename Packet>
inline
typename std::enable_if<
    sizeof(typename Client::element_type::packet_id_t) == 2
>::type
restore_serialized_publish_message(Client const& c, Packet const& packet) {
    c->restore_serialized_message(
        mqtt::publish_message(packet.begin(), packet.end()),
        []{}
    );
}

template <typename Client, typename Packet>
inline
typename std::enable_if<
    sizeof(typename Client::element_type::packet_id_t) == 4
>::type
restore_serialized_publish_message(Client const& c, Packet const& packet) {
    c->restore_serialized_message(
        mqtt::publish_32_message(packet.begin(), packet.end()),
        []{}
    );
}

template <typename Client, typename Packet>
inline
typename std::enable_if<
    sizeof(typename Client::element_type::packet_id_t) == 2
>::type
restore_serialized_pubrel_message(Client const& c, Packet const& packet) {
    c->restore_serialized_message(
        mqtt::pubrel_message(packet.begin(), packet.end())
    );
}

template <typename Client, typename Packet>
inline
typename std::enable_if<
    sizeof(typename Client::element_type::packet_id_t) == 4
>::type
restore_serialized_pubrel_message(Client const& c, Packet const& packet) {
    c->restore_serialized_message(
        mqtt::pubrel_32_message(packet.begin(), packet.end())
    );
}


template <typename Client, typename Serialized>
inline
typename std::enable_if<
    sizeof(typename Client::element_type::packet_id_t) == 2
>::type
set_serialize_handlers(Client const& c, Serialized& serialized) {
    using packet_id_t = typename std::remove_reference_t<decltype(*c)>::packet_id_t;
    c->set_serialize_handlers(
        [&serialized](mqtt::publish_message msg) {
            serialized.emplace(msg.packet_id(), std::make_tuple(true, msg.continuous_buffer()));
        },
        [&serialized](mqtt::pubrel_message msg) {
            BOOST_CHECK(serialized.find(msg.packet_id()) != serialized.end());
            serialized[msg.packet_id()] = std::make_tuple(false, msg.continuous_buffer());
        },
        [&serialized](packet_id_t packet_id) {
            BOOST_CHECK(serialized.find(packet_id) != serialized.end());
            serialized.erase(packet_id);
        }
    );
}

template <typename Client, typename Serialized>
inline
typename std::enable_if<
    sizeof(typename Client::element_type::packet_id_t) == 4
>::type
set_serialize_handlers(Client const& c, Serialized& serialized) {
    using packet_id_t = typename std::remove_reference_t<decltype(*c)>::packet_id_t;
    c->set_serialize_handlers(
        [&serialized](mqtt::publish_32_message msg) {
            serialized.emplace(msg.packet_id(), std::make_tuple(true, msg.continuous_buffer()));
        },
        [&serialized](mqtt::pubrel_32_message msg) {
            BOOST_CHECK(serialized.find(msg.packet_id()) != serialized.end());
            serialized[msg.packet_id()] = std::make_tuple(false, msg.continuous_buffer());
        },
        [&serialized](packet_id_t packet_id) {
            BOOST_CHECK(serialized.find(packet_id) != serialized.end());
            serialized.erase(packet_id);
        }
    );
}


BOOST_AUTO_TEST_CASE( publish_qos1 ) {
    boost::asio::io_service ios;
    test_broker b(ios);
    test_server_no_tls s(ios, b);

    auto c1 = mqtt::make_client(ios, broker_url, broker_notls_port);
    c1->set_client_id("cid1");
    c1->set_clean_session(true);

    auto c2 = mqtt::make_client(ios, broker_url, broker_notls_port);
    c2->set_client_id("cid1");
    c2->set_clean_session(false);

    using packet_id_t = typename std::remove_reference_t<decltype(*c1)>::packet_id_t;

    std::map<
        packet_id_t,
        std::tuple<
            bool,       // is publish
            std::string // whole packet bytes
        >
    > serialized;

    set_serialize_handlers(c1, serialized);
    set_serialize_handlers(c2, serialized);

    std::uint16_t pid_pub;

    std::size_t order = 0;

    std::vector<std::string> const expected = {
        // connect
        "h_connack1",
        // disconnect
        "h_close1",
        // connect
        "h_connack2",
        // publish topic1 QoS1
        // force_disconnect
        "h_error",
        // connect
        "h_connack3",
        "h_puback",
        // disconnect
        "h_close2",
        "finish",
    };

    auto current =
        [&order, &expected]() -> std::string {
            try {
                return expected.at(order);
            }
            catch (std::out_of_range const& e) {
                return e.what();
            }
        };

    c1->set_connack_handler(
        [&order, &current, &c1, &pid_pub]
        (bool sp, std::uint8_t connack_return_code) {
            BOOST_TEST(connack_return_code == mqtt::connect_return_code::accepted);
            switch (order) {
            case 0: // clean session
                BOOST_TEST(current() == "h_connack1");
                ++order;
                BOOST_TEST(sp == false);
                c1->disconnect();
                break;
            case 2:
                BOOST_TEST(current() == "h_connack2");
                ++order;
                BOOST_TEST(sp == false);
                pid_pub = c1->publish_at_least_once("topic1", "topic1_contents");
                c1->force_disconnect();
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
            return true;
        });
    c1->set_close_handler(
        [&order, &current, &c1]
        () {
            switch (order) {
            case 1:
                BOOST_TEST(current() == "h_close1");
                ++order;
                c1->set_clean_session(false);
                c1->connect();
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
        });
    c1->set_error_handler(
        [&order, &current, &c2, &serialized]
        (boost::system::error_code const&) {
            BOOST_TEST(current() == "h_error");
            ++order;
            for (auto const& e : serialized) {
                auto const& packet = std::get<1>(e.second);
                if (std::get<0>(e.second)) {
                    // is publish
                    restore_serialized_publish_message(c2, packet);
                }
                else {
                    // pubrel
                    restore_serialized_pubrel_message(c2, packet);
                }
            }
            c2->connect();
        });

    c2->set_connack_handler(
        [&order, &current]
        (bool sp, std::uint8_t connack_return_code) {
            BOOST_TEST(connack_return_code == mqtt::connect_return_code::accepted);
            switch (order) {
            case 4:
                BOOST_TEST(current() == "h_connack3");
                ++order;
                BOOST_TEST(sp == true);
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
            return true;
        });
    c2->set_close_handler(
        [&order, &current, &s]
        () {
            switch (order) {
            case 6:
                BOOST_TEST(current() == "h_close2");
                ++order;
                s.close();
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
        });
    c2->set_puback_handler(
        [&order, &current, &c2, &pid_pub]
        (packet_id_t packet_id) {
            BOOST_TEST(current() == "h_puback");
            ++order;
            BOOST_TEST(packet_id == pid_pub);
            c2->disconnect();
            return true;
        });

    c1->connect();
    ios.run();
    BOOST_TEST(serialized.empty() == true);
    BOOST_TEST(current() == "finish");
}

BOOST_AUTO_TEST_CASE( publish_qos2 ) {
    boost::asio::io_service ios;
    test_broker b(ios);
    test_server_no_tls s(ios, b);

    auto c1 = mqtt::make_client(ios, broker_url, broker_notls_port);
    c1->set_client_id("cid1");
    c1->set_clean_session(true);

    auto c2 = mqtt::make_client(ios, broker_url, broker_notls_port);
    c2->set_client_id("cid1");
    c2->set_clean_session(false);

    using packet_id_t = typename std::remove_reference_t<decltype(*c1)>::packet_id_t;

    std::map<
        packet_id_t,
        std::tuple<
            bool,       // is publish
            std::string // whole packet bytes
        >
    > serialized;

    set_serialize_handlers(c1, serialized);
    set_serialize_handlers(c2, serialized);

    std::uint16_t pid_pub;

    std::size_t order = 0;

    std::vector<std::string> const expected = {
        // connect
        "h_connack1",
        // disconnect
        "h_close1",
        // connect
        "h_connack2",
        // publish topic1 QoS2
        // force_disconnect
        "h_error",
        // connect
        "h_connack3",
            "h_pubrec",
        "h_pubcomp",
        // disconnect
        "h_close2",
        "finish",
    };

    auto current =
        [&order, &expected]() -> std::string {
            try {
                return expected.at(order);
            }
            catch (std::out_of_range const& e) {
                return e.what();
            }
        };

    c1->set_connack_handler(
        [&order, &current, &c1, &pid_pub]
        (bool sp, std::uint8_t connack_return_code) {
            BOOST_TEST(connack_return_code == mqtt::connect_return_code::accepted);
            switch (order) {
            case 0: // clean session
                BOOST_TEST(current() == "h_connack1");
                ++order;
                BOOST_TEST(sp == false);
                c1->disconnect();
                break;
            case 2:
                BOOST_TEST(current() == "h_connack2");
                ++order;
                BOOST_TEST(sp == false);
                pid_pub = c1->publish_exactly_once("topic1", "topic1_contents");
                c1->force_disconnect();
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
            return true;
        });
    c1->set_close_handler(
        [&order, &current, &c1]
        () {
            switch (order) {
            case 1:
                BOOST_TEST(current() == "h_close1");
                ++order;
                c1->set_clean_session(false);
                c1->connect();
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
        });
    c1->set_error_handler(
        [&order, &current, &c2, &serialized]
        (boost::system::error_code const&) {
            BOOST_TEST(current() == "h_error");
            ++order;
            for (auto const& e : serialized) {
                auto const& packet = std::get<1>(e.second);
                if (std::get<0>(e.second)) {
                    // is publish
                    restore_serialized_publish_message(c2, packet);
                }
                else {
                    // pubrel
                    restore_serialized_pubrel_message(c2, packet);
                }
            }
            c2->connect();
        });

    c2->set_connack_handler(
        [&order, &current]
        (bool sp, std::uint8_t connack_return_code) {
            BOOST_TEST(connack_return_code == mqtt::connect_return_code::accepted);
            switch (order) {
            case 4:
                BOOST_TEST(current() == "h_connack3");
                ++order;
                BOOST_TEST(sp == true);
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
            return true;
        });
    c2->set_close_handler(
        [&order, &current, &s]
        () {
            switch (order) {
            case 7:
                BOOST_TEST(current() == "h_close2");
                ++order;
                s.close();
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
        });
    c2->set_pubrec_handler(
        [&order, &current, &pid_pub]
        (packet_id_t packet_id) {
            BOOST_TEST(current() == "h_pubrec");
            ++order;
            BOOST_TEST(packet_id == pid_pub);
            return true;
        });
    c2->set_pubcomp_handler(
        [&order, &current, &c2, &pid_pub]
        (packet_id_t packet_id) {
            BOOST_TEST(current() == "h_pubcomp");
            ++order;
            BOOST_TEST(packet_id == pid_pub);
            c2->disconnect();
            return true;
        });
    c1->connect();
    ios.run();
    BOOST_TEST(serialized.empty() == true);
    BOOST_TEST(current() == "finish");
}

BOOST_AUTO_TEST_CASE( pubrel_qos2 ) {
    boost::asio::io_service ios;
    test_broker b(ios);
    test_server_no_tls s(ios, b);

    auto c1 = mqtt::make_client(ios, broker_url, broker_notls_port);
    c1->set_client_id("cid1");
    c1->set_clean_session(true);

    auto c2 = mqtt::make_client(ios, broker_url, broker_notls_port);
    c2->set_client_id("cid1");
    c2->set_clean_session(false);

    using packet_id_t = typename std::remove_reference_t<decltype(*c1)>::packet_id_t;

    std::map<
        packet_id_t,
        std::tuple<
            bool,       // is publish
            std::string // whole packet bytes
        >
    > serialized;

    set_serialize_handlers(c1, serialized);
    set_serialize_handlers(c2, serialized);

    std::uint16_t pid_pub;

    std::size_t order = 0;

    std::vector<std::string> const expected = {
        // connect
        "h_connack1",
        // disconnect
        "h_close1",
        // connect
        "h_connack2",
        // publish topic1 QoS2
        "h_pubrec",
        // force_disconnect
        "h_error",
        // connect
        "h_connack3",
        "h_pubcomp",
        // disconnect
        "h_close2",
        "finish",
    };

    auto current =
        [&order, &expected]() -> std::string {
            try {
                return expected.at(order);
            }
            catch (std::out_of_range const& e) {
                return e.what();
            }
        };

    c1->set_connack_handler(
        [&order, &current, &c1, &pid_pub]
        (bool sp, std::uint8_t connack_return_code) {
            BOOST_TEST(connack_return_code == mqtt::connect_return_code::accepted);
            switch (order) {
            case 0: // clean session
                BOOST_TEST(current() == "h_connack1");
                ++order;
                BOOST_TEST(sp == false);
                c1->disconnect();
                break;
            case 2:
                BOOST_TEST(current() == "h_connack2");
                ++order;
                BOOST_TEST(sp == false);
                pid_pub = c1->publish_exactly_once("topic1", "topic1_contents");
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
            return true;
        });
    c1->set_close_handler(
        [&order, &current, &c1]
        () {
            switch (order) {
            case 1:
                BOOST_TEST(current() == "h_close1");
                ++order;
                c1->set_clean_session(false);
                c1->connect();
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
        });
    c1->set_error_handler(
        [&order, &current, &c2, &serialized]
        (boost::system::error_code const&) {
            BOOST_TEST(current() == "h_error");
            ++order;
            for (auto const& e : serialized) {
                auto const& packet = std::get<1>(e.second);
                if (std::get<0>(e.second)) {
                    // is publish
                    restore_serialized_publish_message(c2, packet);
                }
                else {
                    // pubrel
                    restore_serialized_pubrel_message(c2, packet);
                }
            }
            c2->connect();
        });
    c1->set_pubrec_handler(
        [&order, &current, &c1, &pid_pub]
        (packet_id_t packet_id) {
            switch (order) {
            case 3:
                BOOST_TEST(current() == "h_pubrec");
                ++order;
                BOOST_TEST(packet_id == pid_pub);
                c1->force_disconnect();
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
            return true;
        });

    c2->set_connack_handler(
        [&order, &current]
        (bool sp, std::uint8_t connack_return_code) {
            BOOST_TEST(connack_return_code == mqtt::connect_return_code::accepted);
            switch (order) {
            case 5:
                BOOST_TEST(current() == "h_connack3");
                ++order;
                BOOST_TEST(sp == true);
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
            return true;
        });
    c2->set_close_handler(
        [&order, &current, &s]
        () {
            switch (order) {
            case 7:
                BOOST_TEST(current() == "h_close2");
                ++order;
                s.close();
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
        });
    c2->set_pubcomp_handler(
        [&order, &current, &c2]
        (packet_id_t packet_id) {
            BOOST_TEST(current() == "h_pubcomp");
            ++order;
            BOOST_TEST(packet_id == 1);
            c2->disconnect();
            return true;
        });
    c1->connect();
    ios.run();
    BOOST_TEST(serialized.empty() == true);
    BOOST_TEST(current() == "finish");
}

BOOST_AUTO_TEST_CASE( multi_publish_qos1 ) {
    boost::asio::io_service ios;
    test_broker b(ios);
    test_server_no_tls s(ios, b);

    auto c1 = mqtt::make_client(ios, broker_url, broker_notls_port);
    c1->set_client_id("cid1");
    c1->set_clean_session(true);

    auto c2 = mqtt::make_client(ios, broker_url, broker_notls_port);
    c2->set_client_id("cid1");
    c2->set_clean_session(false);

    using packet_id_t = typename std::remove_reference_t<decltype(*c1)>::packet_id_t;

    std::map<
        packet_id_t,
        std::tuple<
            bool,       // is publish
            std::string // whole packet bytes
        >
    > serialized;

    set_serialize_handlers(c1, serialized);
    set_serialize_handlers(c2, serialized);

    std::uint16_t pid_pub1;
    std::uint16_t pid_pub2;

    std::size_t order = 0;

    std::vector<std::string> const expected = {
        // connect
        "h_connack1",
        // disconnect
        "h_close1",
        // connect
        "h_connack2",
        // publish topic1 QoS1
        // publish topic1 QoS1
        // force_disconnect
        "h_error1",
        // connect
        "h_connack3",
        "h_puback1",
        "h_puback2",
        // disconnect
        "h_close2",
        "finish",
    };

    auto current =
        [&order, &expected]() -> std::string {
            try {
                return expected.at(order);
            }
            catch (std::out_of_range const& e) {
                return e.what();
            }
        };

    c1->set_connack_handler(
        [&order, &current, &c1, &pid_pub1, &pid_pub2]
        (bool sp, std::uint8_t connack_return_code) {
            BOOST_TEST(connack_return_code == mqtt::connect_return_code::accepted);
            switch (order) {
            case 0: // clean session
                BOOST_TEST(current() == "h_connack1");
                ++order;
                BOOST_TEST(sp == false);
                c1->disconnect();
                break;
            case 2:
                BOOST_TEST(current() == "h_connack2");
                ++order;
                BOOST_TEST(sp == false);
                pid_pub1 = c1->publish_at_least_once("topic1", "topic1_contents1");
                pid_pub2 = c1->publish_at_least_once("topic1", "topic1_contents2");
                c1->force_disconnect();
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
            return true;
        });
    c1->set_close_handler(
        [&order, &current, &c1]
        () {
            switch (order) {
            case 1:
                BOOST_TEST(current() == "h_close1");
                ++order;
                c1->set_clean_session(false);
                c1->connect();
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
        });
    c1->set_error_handler(
        [&order, &current, &c2, &serialized]
        (boost::system::error_code const&) {
            switch (order) {
            case 3:
                BOOST_TEST(current() == "h_error1");
                ++order;
                for (auto const& e : serialized) {
                    auto const& packet = std::get<1>(e.second);
                    if (std::get<0>(e.second)) {
                        // is publish
                        restore_serialized_publish_message(c2, packet);
                    }
                    else {
                        // pubrel
                        restore_serialized_pubrel_message(c2, packet);
                    }
                }
                c2->connect();
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
        });
    c2->set_connack_handler(
        [&order, &current]
        (bool sp, std::uint8_t connack_return_code) {
            BOOST_TEST(connack_return_code == mqtt::connect_return_code::accepted);
            switch (order) {
            case 4:
                BOOST_TEST(current() == "h_connack3");
                ++order;
                BOOST_TEST(sp == true);
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
            return true;
        });
    c2->set_close_handler(
        [&order, &current, &s]
        () {
            switch (order) {
            case 7:
                BOOST_TEST(current() == "h_close2");
                ++order;
                s.close();
                break;
            default:
                BOOST_CHECK(false);
                break;
            }
        });
    c2->set_puback_handler(
        [&order, &current, &c2, &pid_pub1, &pid_pub2]
        (packet_id_t packet_id) {
            switch (order) {
            case 5:
                BOOST_TEST(current() == "h_puback1");
                ++order;
                BOOST_TEST(packet_id == pid_pub1);
                break;
            case 6:
                BOOST_TEST(current() == "h_puback2");
                ++order;
                BOOST_TEST(packet_id == pid_pub2);
                c2->disconnect();
                break;
            }
            return true;
        });
    c1->connect();
    ios.run();
    BOOST_TEST(serialized.empty() == true);
    BOOST_TEST(current() == "finish");
}

BOOST_AUTO_TEST_SUITE_END()
