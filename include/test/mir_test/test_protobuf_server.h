/*
 * Copyright © 2012 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Thomas Voss <thomas.voss@canonical.com>
 *              Alan Griffiths <alan@octopull.co.uk>
 */
#ifndef MIR_TEST_TEST_PROTOBUF_SERVER_H_
#define MIR_TEST_TEST_PROTOBUF_SERVER_H_

#include <memory>

namespace mir
{
namespace frontend
{
class Communicator;
class CommunicatorReport;
class SocketConnection;
}

namespace protobuf
{
class DisplayServer;
}

namespace test
{
struct TestProtobufServer
{
    TestProtobufServer(
        std::string const& socket_name,
        std::shared_ptr<protobuf::DisplayServer> const& tool);

    TestProtobufServer(
        std::shared_ptr<frontend::SocketConnection> const& socket_connection,
        std::shared_ptr<protobuf::DisplayServer> const& tool);

    TestProtobufServer(
        std::string const& socket_name,
        std::shared_ptr<protobuf::DisplayServer> const& tool,
        std::shared_ptr<frontend::CommunicatorReport> const& report);

    TestProtobufServer(
        std::shared_ptr<frontend::SocketConnection> const& socket_connection,
        std::shared_ptr<protobuf::DisplayServer> const& tool,
        std::shared_ptr<frontend::CommunicatorReport> const& report);

    // "Server" side
    std::shared_ptr<frontend::Communicator> const comm;
};
}
}
#endif /* MIR_TEST_TEST_PROTOBUF_SERVER_H_ */
