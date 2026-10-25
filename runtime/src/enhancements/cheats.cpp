#include "enhancements/cheats.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "core/config.h"
#include "core/guest.h"
#include "core/log.h"
#include "ui/settings.h"

namespace wf::cheats {
namespace {

struct Write
{
    bool viaPointer = false;
    uint32_t addr = 0;
    uint32_t offset = 0;
    int size = 4;
    bool isFloat = false;
    bool copy = false;
    uint32_t source = 0;
    uint32_t value = 0;
};

struct Entry
{
    Cheat info;
    std::vector<Write> writes;
};

std::once_flag g_once;
std::vector<Entry> g_entries;
std::vector<Cheat> g_list;

std::string trim(std::string s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

uint32_t number(const std::string& s) { return (uint32_t)std::strtoull(trim(s).c_str(), nullptr, 0); }

bool parse_write(const std::string& line, Write& w)
{
    size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    std::string target = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
    if (target.empty() || value.empty() || value[0] != '.') return false;
    if (target[0] == '*')
    {
        w.viaPointer = true;
        size_t plus = target.find('+');
        w.addr = number(target.substr(1, plus == std::string::npos ? std::string::npos : plus - 1));
        if (plus != std::string::npos) w.offset = number(target.substr(plus + 1));
    }
    else
        w.addr = number(target);
    size_t sp = value.find_first_of(" \t");
    if (sp == std::string::npos) return false;
    std::string kind = value.substr(1, sp - 1), v = trim(value.substr(sp));
    if (kind == "byte" || kind == "u8") w.size = 1;
    else if (kind == "short" || kind == "u16") w.size = 2;
    else if (kind == "int" || kind == "uint" || kind == "u32") w.size = 4;
    else if (kind == "float") { w.size = 4; w.isFloat = true; }
    else return false;
    if (!v.empty() && v[0] == '@')
    {
        w.copy = true;
        w.source = number(v.substr(1));
    }
    else if (w.isFloat)
    {
        float f = (float)std::atof(v.c_str());
        std::memcpy(&w.value, &f, 4);
    }
    else
        w.value = (uint32_t)std::strtoll(v.c_str(), nullptr, 0);
    return true;
}

void parse_file(const std::string& text, const std::string& source)
{
    Entry* cur = nullptr;
    std::istringstream in(text);
    for (std::string raw; std::getline(in, raw);)
    {
        std::string line = raw;
        if (size_t c = line.find(';'); c != std::string::npos) line = line.substr(0, c);
        if (size_t c = line.find('#'); c != std::string::npos) line = line.substr(0, c);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']')
        {
            g_entries.push_back({{trim(line.substr(1, line.size() - 2)), "", source}, {}});
            cur = &g_entries.back();
            continue;
        }
        if (!cur) continue;
        if (line.rfind("description", 0) == 0 && line.find('=') != std::string::npos)
        {
            cur->info.description = trim(raw.substr(raw.find('=') + 1));
            continue;
        }
        Write w;
        if (parse_write(line, w)) cur->writes.push_back(w);
        else log_warn("cheats: {}: can't read \"{}\"", source, trim(raw));
    }
}

constexpr const char* kBuiltIn = "";

void load()
{
    parse_file(kBuiltIn, "built-in");
    std::error_code ec;
    std::filesystem::path dir = config().dataDir / "cheats";
    if (const wchar_t* e = _wgetenv(L"WF_CHEATS_DIR")) dir = e;
    if (std::filesystem::exists(dir, ec))
        for (const auto& f : std::filesystem::directory_iterator(dir, ec))
            if (f.path().extension() == ".txt")
            {
                std::ifstream in(f.path());
                parse_file(std::string(std::istreambuf_iterator<char>(in), {}), f.path().filename().string());
            }
    for (const Entry& e : g_entries) g_list.push_back(e.info);
    log_info("cheats: {} available", g_entries.size());
}

uint32_t read_sized(uint32_t addr, int size)
{
    return size == 1 ? rd8(addr) : size == 2 ? rd16(addr) : rd32(addr);
}

}

const std::vector<Cheat>& list()
{
    std::call_once(g_once, load);
    return g_list;
}

void apply_frame()
{
    std::call_once(g_once, load);
    std::map<std::string, bool> enabled;
    {
        std::lock_guard lk(settings::lock());
        enabled = settings::get().cheats;
    }
    for (const Entry& e : g_entries)
    {
        auto it = enabled.find(e.info.name);
        if (it == enabled.end() || !it->second) continue;
        for (const Write& w : e.writes)
        {
            uint32_t addr = w.addr;
            if (w.viaPointer)
            {
                if (!is_mapped(w.addr, 4)) continue;
                uint32_t ptr = rd32(w.addr);
                if (!ptr) continue;
                addr = ptr + w.offset;
            }
            if (!is_mapped(addr, (uint32_t)w.size) || (w.copy && !is_mapped(w.source, (uint32_t)w.size))) continue;
            uint32_t v = w.copy ? read_sized(w.source, w.size) : w.value;
            if (w.size == 1) wr8(addr, (uint8_t)v);
            else if (w.size == 2) wr16(addr, (uint16_t)v);
            else wr32(addr, v);
        }
    }
}

}
