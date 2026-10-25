#pragma once
#include <format>
#include <string>
#include <string_view>

namespace wf {

enum class LogLevel { Trace, Info, Warn, Error };

void log_write(LogLevel level, std::string_view category, std::string_view msg);
bool log_enabled(std::string_view category);
[[noreturn]] void fatal_message(std::string_view msg);

template<class... A> void log_info(std::format_string<A...> f, A&&... a) { log_write(LogLevel::Info, "", std::format(f, std::forward<A>(a)...)); }
template<class... A> void log_warn(std::format_string<A...> f, A&&... a) { log_write(LogLevel::Warn, "", std::format(f, std::forward<A>(a)...)); }
template<class... A> void log_error(std::format_string<A...> f, A&&... a) { log_write(LogLevel::Error, "", std::format(f, std::forward<A>(a)...)); }
template<class... A> void log_cat(std::string_view cat, std::format_string<A...> f, A&&... a)
{
    if (log_enabled(cat)) log_write(LogLevel::Info, cat, std::format(f, std::forward<A>(a)...));
}
template<class... A> [[noreturn]] void fatal(std::format_string<A...> f, A&&... a) { fatal_message(std::format(f, std::forward<A>(a)...)); }

}
