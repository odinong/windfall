#include "Common/precompiled.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/GraphicPack/GraphicPack2.h"

#include "enhancements/graphic_packs.h"

#include <cmath>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>

#include "core/cemu_install.h"
#include "core/config.h"
#include "core/guest.h"
#include "core/log.h"
#include "gpu/window.h"
#include "ui/settings.h"

namespace wf::gp {
namespace fs = std::filesystem;

namespace {

constexpr const char* kTitleId = "0005000010143500";
constexpr uint32_t kModuleUSA = 0x475BD29F;

std::string trim(std::string s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string strip_comment(const std::string& line)
{
    bool quoted = false;
    for (size_t i = 0; i < line.size(); i++)
    {
        if (line[i] == '"') quoted = !quoted;
        else if (!quoted && (line[i] == '#' || line[i] == ';')) return line.substr(0, i);
    }
    return line;
}

std::string unquote(std::string s)
{
    s = trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

std::string var_name(const std::string& key)
{
    std::string k = trim(key);
    if (!k.empty() && k[0] == '$') k = k.substr(1);
    if (size_t c = k.find(':'); c != std::string::npos) k = k.substr(0, c);
    return trim(k);
}

bool var_is_int(const std::string& key) { return key.find(":int") != std::string::npos; }

struct Evaluator
{
    const std::map<std::string, std::string>& exprs;
    std::map<std::string, double> cache;
    std::set<std::string> active;
    const char* p = nullptr;

    explicit Evaluator(const std::map<std::string, std::string>& e) : exprs(e) {}

    double var(const std::string& name)
    {
        if (auto c = cache.find(name); c != cache.end()) return c->second;
        auto it = exprs.find(name);
        if (it == exprs.end() || active.count(name)) return 0.0;
        active.insert(name);
        const char* saved = p;
        double v = eval(it->second);
        p = saved;
        active.erase(name);
        cache[name] = v;
        return v;
    }

    double eval(const std::string& s)
    {
        p = s.c_str();
        return comparison();
    }

    void ws() { while (*p == ' ' || *p == '\t') p++; }

    double comparison()
    {
        double a = additive();
        for (;;)
        {
            ws();
            if (p[0] == '<' && p[1] == '=') { p += 2; a = a <= additive(); }
            else if (p[0] == '>' && p[1] == '=') { p += 2; a = a >= additive(); }
            else if (p[0] == '=' && p[1] == '=') { p += 2; a = a == additive(); }
            else if (p[0] == '!' && p[1] == '=') { p += 2; a = a != additive(); }
            else if (*p == '<') { p++; a = a < additive(); }
            else if (*p == '>') { p++; a = a > additive(); }
            else return a;
        }
    }

    double additive()
    {
        double a = multiplicative();
        for (;;)
        {
            ws();
            if (*p == '+') { p++; a += multiplicative(); }
            else if (*p == '-') { p++; a -= multiplicative(); }
            else return a;
        }
    }

    double multiplicative()
    {
        double a = unary();
        for (;;)
        {
            ws();
            if (*p == '*') { p++; a *= unary(); }
            else if (*p == '/') { p++; double b = unary(); a = b != 0 ? a / b : 0.0; }
            else if (*p == '%') { p++; double b = unary(); a = b != 0 ? std::fmod(a, b) : 0.0; }
            else return a;
        }
    }

    double unary()
    {
        ws();
        if (*p == '-') { p++; return -unary(); }
        if (*p == '+') { p++; return unary(); }
        return primary();
    }

    double primary()
    {
        ws();
        if (*p == '(')
        {
            p++;
            double v = comparison();
            ws();
            if (*p == ')') p++;
            return v;
        }
        if (*p == '$')
        {
            p++;
            std::string n;
            while (std::isalnum((unsigned char)*p) || *p == '_') n += *p++;
            return var(n);
        }
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        {
            char* end = nullptr;
            double v = (double)std::strtoull(p, &end, 16);
            p = end;
            return v;
        }
        if (std::isdigit((unsigned char)*p) || *p == '.')
        {
            char* end = nullptr;
            double v = std::strtod(p, &end);
            p = end;
            return v;
        }
        std::string f;
        while (std::isalpha((unsigned char)*p)) f += *p++;
        if (!f.empty())
        {
            ws();
            if (*p == '(')
            {
                p++;
                double a = comparison(), b = 0;
                ws();
                if (*p == ',') { p++; b = comparison(); ws(); }
                if (*p == ')') p++;
                if (f == "min") return std::min(a, b);
                if (f == "max") return std::max(a, b);
                if (f == "floor") return std::floor(a);
                if (f == "ceil") return std::ceil(a);
                if (f == "round") return std::round(a);
                if (f == "abs") return std::abs(a);
                return a;
            }
        }
        if (*p) p++;
        return 0.0;
    }
};

std::mutex g_lock;
bool g_scanned = false;
std::vector<Pack> g_packs;
std::map<std::tuple<uint64_t, uint64_t, int>, std::string> g_shaders;
struct DataPatch { uint32_t addr; std::string kind; std::string expr; };
std::vector<std::pair<const Pack*, std::vector<DataPatch>>> g_patches;

void parse_rules(Pack& pk, const fs::path& rules)
{
    std::ifstream f(rules);
    std::string section;
    Preset* preset = nullptr;
    std::map<std::string, std::string>* rule = nullptr;
    bool inDefinition = false, inDefault = false;
    for (std::string raw; std::getline(f, raw);)
    {
        std::string line = trim(strip_comment(raw));
        if (line.empty()) continue;
        if (line.front() == '[')
        {
            section = lower(trim(line.substr(1, line.find(']') - 1)));
            inDefinition = section == "definition";
            inDefault = section == "default";
            preset = nullptr;
            rule = nullptr;
            if (section == "preset")
            {
                pk.presets.emplace_back();
                preset = &pk.presets.back();
            }
            else if (section == "textureredefine")
            {
                pk.textureRules.emplace_back();
                rule = &pk.textureRules.back();
            }
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (inDefinition)
        {
            std::string k = lower(key);
            if (k == "name") pk.name = unquote(value);
            else if (k == "path") pk.id = unquote(value);
            else if (k == "description") pk.description = unquote(trim(strip_comment(raw.substr(raw.find('=') + 1))));
        }
        else if (inDefault && key[0] == '$')
            pk.defaults.emplace_back(key, value);
        else if (preset)
        {
            std::string k = lower(key);
            if (key[0] == '$') preset->vars.emplace_back(key, value);
            else if (k == "name") preset->name = unquote(value);
            else if (k == "category") preset->category = unquote(value);
            else if (k == "default") preset->isDefault = value == "1" || lower(value) == "true";
        }
        else if (rule)
            (*rule)[lower(key)] = value;
    }
    std::replace(pk.description.begin(), pk.description.end(), '|', '\n');
    for (const Preset& p : pk.presets)
        if (std::find(pk.categories.begin(), pk.categories.end(), p.category) == pk.categories.end())
            pk.categories.push_back(p.category);
}

bool for_this_game(const fs::path& rules)
{
    std::ifstream f(rules);
    for (std::string line; std::getline(f, line);)
    {
        std::string l = lower(line);
        if (l.find("titleids") != std::string::npos) return l.find(kTitleId) != std::string::npos;
    }
    return false;
}

struct PatchSection { bool usa = false; std::vector<DataPatch> data; int code = 0; };

std::vector<PatchSection> parse_patches(const fs::path& file)
{
    std::vector<PatchSection> sections;
    std::ifstream f(file);
    for (std::string raw; std::getline(f, raw);)
    {
        std::string line = trim(strip_comment(raw));
        if (line.empty()) continue;
        if (line.front() == '[') { sections.emplace_back(); continue; }
        if (sections.empty()) continue;
        PatchSection& s = sections.back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) { s.code++; continue; }
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        std::string k = lower(key);
        if (k == "modulematches")
        {
            std::stringstream ss(value);
            for (std::string m; std::getline(ss, m, ',');)
                if ((uint32_t)std::strtoul(trim(m).c_str(), nullptr, 16) == kModuleUSA) s.usa = true;
            continue;
        }
        if (k == "codecavesize") { s.code++; continue; }
        if (k.rfind("0x", 0) != 0) continue;
        uint32_t addr = (uint32_t)std::strtoul(key.c_str(), nullptr, 16);
        if (!value.empty() && value[0] == '.')
        {
            size_t sp = value.find_first_of(" \t");
            std::string kind = lower(value.substr(1, sp == std::string::npos ? std::string::npos : sp - 1));
            std::string expr = sp == std::string::npos ? "" : trim(value.substr(sp));
            if (addr >= 0x10000000 && (kind == "float" || kind == "double" || kind == "int" || kind == "uint" || kind == "short" ||
                                       kind == "byte" || kind == "string" || kind == "u32" || kind == "u16" || kind == "u8"))
            {
                s.data.push_back({addr, kind, expr});
                continue;
            }
        }
        s.code++;
    }
    return sections;
}

void scan_locked()
{
    if (g_scanned) return;
    g_scanned = true;
    std::vector<fs::path> roots;
    roots.push_back(config().dataDir / "graphicPacks");
    if (auto cemu = cemu_install_dir()) roots.push_back(*cemu / "graphicPacks");
    std::set<std::string> seen;
    std::error_code ec;
    for (const fs::path& root : roots)
    {
        if (!fs::exists(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
        {
            if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
            if (it->path().filename() != "rules.txt" || !for_this_game(it->path())) continue;
            Pack pk;
            pk.dir = it->path().parent_path();
            parse_rules(pk, it->path());
            if (pk.id.empty()) pk.id = pk.name;
            if (pk.name.empty() || !seen.insert(pk.id).second) continue;
            for (const auto& f : fs::directory_iterator(pk.dir, ec))
            {
                std::string n = f.path().filename().string();
                static const std::regex shaderName("^[0-9a-f]{16}_[0-9a-f]{16}_(vs|ps|gs)\\.txt$");
                if (std::regex_match(n, shaderName)) pk.shaderCount++;
            }
            if (fs::exists(pk.dir / "patches.txt", ec))
                for (const PatchSection& s : parse_patches(pk.dir / "patches.txt"))
                    if (s.usa)
                    {
                        pk.dataPatchCount += (int)s.data.size();
                        pk.codePatchCount += s.code;
                    }
            g_packs.push_back(std::move(pk));
        }
        ec.clear();
    }
    std::sort(g_packs.begin(), g_packs.end(), [](const Pack& a, const Pack& b) { return a.id < b.id; });
    log_info("graphic packs: {} for this game", g_packs.size());
}

std::map<std::string, std::string> pack_vars(const Pack& pk, const settings::Settings& s, std::set<std::string>* ints)
{
    std::map<std::string, std::string> vars;
    auto add = [&](const std::vector<std::pair<std::string, std::string>>& list) {
        for (const auto& [k, v] : list)
        {
            vars[var_name(k)] = v;
            if (ints && var_is_int(k)) ints->insert(var_name(k));
        }
    };
    add(pk.defaults);
    for (const std::string& cat : pk.categories)
    {
        std::string chosen = settings::pack_preset(s, pk.id, cat);
        bool matchWindow = chosen == kMatchWindow;
        const Preset* sel = nullptr;
        for (const Preset& p : pk.presets)
            if (p.category == cat && !matchWindow && p.name == chosen) sel = &p;
        if (!sel)
            for (const Preset& p : pk.presets)
                if (p.category == cat && p.isDefault && !sel) sel = &p;
        if (!sel)
            for (const Preset& p : pk.presets)
                if (p.category == cat && !sel) sel = &p;
        if (sel) add(sel->vars);
        if (matchWindow)
        {
            uint32_t h = window::state().height, w = window::state().width;
            if (h == 0) h = 1080;
            uint32_t height = h, width = (uint32_t)std::lround(height * 16.0 / 9.0);
            if (width > w && w) { width = w; height = (uint32_t)std::lround(width * 9.0 / 16.0); }
            vars["width"] = std::to_string(width);
            vars["height"] = std::to_string(height);
        }
    }
    return vars;
}

std::string format_value(double v, bool isInt)
{
    if (isInt || v == std::floor(v) && std::abs(v) < 1e9) return std::to_string((long long)std::llround(v));
    return fmt::format("{}", v);
}

}

const std::vector<Pack>& packs()
{
    std::lock_guard lk(g_lock);
    scan_locked();
    return g_packs;
}

const Pack* find(const std::string& id)
{
    for (const Pack& p : packs())
        if (p.id == id) return &p;
    return nullptr;
}

const Pack* resolution_pack()
{
    for (const Pack& p : packs())
        if (p.id.ends_with("/Graphics/Resolution")) return &p;
    return nullptr;
}

void activate()
{
    settings::Settings s;
    {
        std::lock_guard lk(settings::lock());
        s = settings::get();
    }
    const std::vector<Pack>& all = packs();
    std::lock_guard lk(g_lock);
    for (const Pack& pk : all)
    {
        if (!settings::pack_enabled(s, pk.id) || !pk.supported()) continue;
        std::set<std::string> ints;
        std::map<std::string, std::string> vars = pack_vars(pk, s, &ints);
        Evaluator ev(vars);

        auto gp = std::make_shared<GraphicPack2>();
        gp->m_name = pk.name;
        for (const auto& r : pk.textureRules)
        {
            GraphicPack2::TextureRule rule;
            auto num = [&](const char* key, sint32& out, double scale = 1.0) {
                if (auto it = r.find(key); it != r.end()) out = (sint32)std::lround(ev.eval(it->second) * scale);
            };
            auto list = [&](const char* key, std::vector<sint32>& out) {
                if (auto it = r.find(key); it != r.end())
                {
                    std::stringstream ss(it->second);
                    for (std::string v; std::getline(ss, v, ',');)
                        if (!trim(v).empty()) out.push_back((sint32)std::strtol(trim(v).c_str(), nullptr, 0));
                }
            };
            num("width", rule.filter_settings.width);
            num("height", rule.filter_settings.height);
            num("depth", rule.filter_settings.depth);
            list("formats", rule.filter_settings.format_whitelist);
            list("formatsexcluded", rule.filter_settings.format_blacklist);
            list("tilemodes", rule.filter_settings.tilemode_whitelist);
            list("tilemodesexcluded", rule.filter_settings.tilemode_blacklist);
            if (auto it = r.find("inmem1"); it != r.end())
                rule.filter_settings.inMEM1 = lower(it->second) == "true" || it->second == "1" ? GraphicPack2::TextureRule::FILTER_SETTINGS::MEM1_FILTER::INSIDE
                                                                                              : GraphicPack2::TextureRule::FILTER_SETTINGS::MEM1_FILTER::OUTSIDE;
            num("overwritewidth", rule.overwrite_settings.width);
            num("overwriteheight", rule.overwrite_settings.height);
            num("overwritedepth", rule.overwrite_settings.depth);
            num("overwriteformat", rule.overwrite_settings.format);
            num("overwritelodbias", rule.overwrite_settings.lod_bias, 64.0);
            num("overwriterelativelodbias", rule.overwrite_settings.relative_lod_bias, 64.0);
            if (auto it = r.find("overwriteanisotropy"); it != r.end())
            {
                double level = ev.eval(it->second);
                rule.overwrite_settings.anistropic_value = std::clamp((int)std::lround(std::log2(std::max(1.0, level))), 0, 4);
            }
            gp->m_textureRules.push_back(rule);
        }
        GraphicPack2::s_active.push_back(gp);

        std::vector<std::string> names;
        for (const auto& [n, _] : vars) names.push_back(n);
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        std::error_code ec;
        for (const auto& f : fs::directory_iterator(pk.dir, ec))
        {
            std::string n = f.path().filename().string();
            static const std::regex shaderName("^([0-9a-f]{16})_([0-9a-f]{16})_(vs|ps|gs)\\.txt$");
            std::smatch m;
            if (!std::regex_match(n, m, shaderName)) continue;
            std::ifstream in(f.path());
            std::string src((std::istreambuf_iterator<char>(in)), {});
            for (const std::string& v : names)
            {
                std::string token = "$" + v, value = format_value(ev.var(v), ints.count(v) > 0);
                for (size_t pos = 0; (pos = src.find(token, pos)) != std::string::npos;)
                {
                    src.replace(pos, token.size(), value);
                    pos += value.size();
                }
            }
            int type = m[3] == "vs" ? 0 : m[3] == "ps" ? 1 : 2;
            g_shaders[{std::stoull(m[1], nullptr, 16), std::stoull(m[2], nullptr, 16), type}] = std::move(src);
        }

        if (fs::exists(pk.dir / "patches.txt", ec))
        {
            std::vector<DataPatch> data;
            for (PatchSection& sec : parse_patches(pk.dir / "patches.txt"))
                if (sec.usa) data.insert(data.end(), sec.data.begin(), sec.data.end());
            for (DataPatch& d : data)
                if (d.kind != "string") d.expr = fmt::format("{:.17g}", ev.eval(d.expr));
            if (!data.empty()) g_patches.push_back({&pk, std::move(data)});
        }
        log_info("graphic pack: {} ({} texture rules, {} shaders, {} data patches)", pk.name, gp->m_textureRules.size(), pk.shaderCount, pk.dataPatchCount);
    }
}

void apply_data_patches()
{
    std::lock_guard lk(g_lock);
    for (const auto& [pk, list] : g_patches)
        for (const DataPatch& d : list)
        {
            if (!is_mapped(d.addr, 8)) continue;
            double v = std::strtod(d.expr.c_str(), nullptr);
            if (d.kind == "float") wr32(d.addr, std::bit_cast<uint32_t>((float)v));
            else if (d.kind == "double")
            {
                uint64_t b = std::bit_cast<uint64_t>(v);
                wr32(d.addr, (uint32_t)(b >> 32));
                wr32(d.addr + 4, (uint32_t)b);
            }
            else if (d.kind == "int" || d.kind == "uint" || d.kind == "u32") wr32(d.addr, (uint32_t)(int64_t)v);
            else if (d.kind == "short" || d.kind == "u16") wr16(d.addr, (uint16_t)(int64_t)v);
            else if (d.kind == "byte" || d.kind == "u8") wr8(d.addr, (uint8_t)(int64_t)v);
            else if (d.kind == "string")
            {
                std::string str = unquote(d.expr);
                for (size_t i = 0; i <= str.size(); i++) wr8(d.addr + (uint32_t)i, i < str.size() ? (uint8_t)str[i] : 0);
            }
        }
    if (!g_patches.empty()) log_info("graphic packs: data patches applied");
}

const std::string* find_custom_shader(uint64_t baseHash, uint64_t auxHash, int type)
{
    std::lock_guard lk(g_lock);
    if (auto it = g_shaders.find({baseHash, auxHash, type}); it != g_shaders.end()) return &it->second;
    return nullptr;
}

}

const std::string* wf_find_custom_shader(uint64 baseHash, uint64 auxHash, int type)
{
    return wf::gp::find_custom_shader(baseHash, auxHash, type);
}
