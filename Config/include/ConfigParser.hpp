#ifndef CONFIG_PARSER_HPP
#define CONFIG_PARSER_HPP

#include "AppConfig.hpp"

#include <iosfwd>
#include <string_view>

namespace telemetry::config
{

enum class ParseStatus { Success, HelpRequested, Error };

[[nodiscard]]
ParseStatus parseArguments(int argc, char* const argv[], AppConfig &config,
                           std::ostream &errorOutput);

void printUsage(std::ostream &output, std::string_view programName);

} // namespace telemetry::config

#endif