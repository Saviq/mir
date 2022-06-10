/*
 * Copyright © 2022 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 or 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MIR_TEST_DOUBLES_FILE_LOGGER_H_
#define MIR_TEST_DOUBLES_FILE_LOGGER_H_

#include "mir/logging/logger.h"

#include <fstream>
#include <string>

namespace mir
{
namespace test
{
namespace doubles
{
class FileLogger : public mir::logging::Logger
{
public:
    FileLogger(const std::string& filename);

protected:
    void log(mir::logging::Severity severity, const std::string& message, const std::string& component) override;

private:
    std::unique_ptr<std::ofstream> out;
};
}
}
}

#endif // MIR_TEST_DOUBLES_FILE_LOGGER_H_
