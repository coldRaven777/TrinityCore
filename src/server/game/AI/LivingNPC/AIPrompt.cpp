/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 */

#include "AI/LivingNPC/AIPrompt.h"
#include "AI/LivingNPC/AIConfig.h"

#include "Log.h"
#include "GameTime.h"

#include <fstream>
#include <sstream>
#include <set>
#include <ctime>
#include <cctype>
#include <algorithm>

namespace
{
    using CacheEntry = std::pair<fs::file_time_type, std::string>;

    // path -> (mtime, content) template cache; bios share this cache (keyed by path).
    std::map<std::string, CacheEntry> g_cache;
    // creature entry -> bio file path (rebuilt by IndexBios / Reload).
    std::map<uint32, std::string> g_bioIndex;
    // placeholders we already warned about (avoid log spam).
    std::set<std::string> g_warnedPlaceholders;

    bool Exists(fs::path const& p)
    {
        std::error_code ec;
        bool ok = fs::exists(p, ec);
        return ok && !ec;
    }

    std::string Trim(std::string s)
    {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    std::string SanitizeName(std::string const& name)
    {
        std::string out;
        out.reserve(name.size());
        for (char ch : name)
        {
            if (ch < 0x20 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
                ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
                continue;
            out += ch;
        }
        // collapse runs of spaces and trim
        std::string collapsed;
        bool prevSpace = false;
        for (char ch : out)
        {
            if (ch == ' ')
            {
                if (prevSpace) continue;
                prevSpace = true;
            }
            else
                prevSpace = false;
            collapsed += ch;
        }
        return Trim(collapsed);
    }

    std::string CurrentTimeStr()
    {
        time_t t = GameTime::GetGameTime();
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
        return buf;
    }

    char const* BuiltinSystem()
    {
        return
            "You are {{npc}}, a creature in the world of Warcraft.\n"
            "Stay fully in character. Never mention that you are an AI or break character.\n"
            "Respond naturally and concisely (one or two sentences).\n"
            "Always reply in the language '{{language}}'.";
    }

    char const* BuiltinReinforcement()
    {
        return
            "Stay in character as {{npc}}. Take into consideration that the current time is "
            "{{time}} and this conversation takes place in {{location}}. Always reply in {{language}}.";
    }

    void WriteFile(fs::path const& p, std::string const& content)
    {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p);
        f << content;
    }
}

fs::path AIPrompt::PromptRoot()
{
    fs::path root(sAIConfig->PromptRoot());
    if (!root.is_absolute())
        root = fs::current_path() / root;
    return root;
}

std::string AIPrompt::ReadCached(std::string const& path)
{
    std::error_code ec;
    auto mtime = fs::last_write_time(path, ec);
    if (ec)
        return ""; // missing / unreadable -> treat as empty

    auto it = g_cache.find(path);
    if (it != g_cache.end() && it->second.first == mtime)
        return it->second.second;

    std::ifstream f(path);
    if (!f.is_open())
        return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();
    g_cache[path] = { mtime, content };
    return content;
}

std::string AIPrompt::LoadSlot(std::string const& type, std::string const& slot)
{
    fs::path p = PromptRoot() / "prompts" / type / slot;
    if (Exists(p))
        return ReadCached(p.string());

    p = PromptRoot() / "prompts" / "shared" / slot;
    if (Exists(p))
        return ReadCached(p.string());

    if (slot == "system.md")
        return BuiltinSystem();
    if (slot == "reinforcement.md")
        return BuiltinReinforcement();
    return "";
}

std::vector<std::pair<std::string, std::string>> AIPrompt::LoadSlots(std::string const& type)
{
    std::vector<std::pair<std::string, std::string>> slots;
    fs::path manifest = PromptRoot() / "prompts" / type / "slots.txt";
    if (Exists(manifest))
    {
        std::ifstream f(manifest);
        std::string line;
        while (std::getline(f, line))
        {
            std::string t = Trim(line);
            if (t.empty() || t[0] == '#')
                continue;
            size_t sep = t.find(':');
            if (sep == std::string::npos)
                continue;
            slots.emplace_back(Trim(t.substr(0, sep)), Trim(t.substr(sep + 1)));
        }
    }
    if (slots.empty())
    {
        slots.emplace_back("system.md", "system");
        slots.emplace_back("bio.md", "user");
        slots.emplace_back("vitals.md", "user");
        slots.emplace_back("memories.md", "user");
        slots.emplace_back("summary.md", "user");
        slots.emplace_back("events.md", "user");
        slots.emplace_back("reinforcement.md", "user");
    }
    return slots;
}

std::string AIPrompt::Resolve(std::string const& tmpl, AIPromptInput const& in,
                              std::string const& bio, std::string const& memories,
                              std::string const& summary, std::string const& events)
{
    if (tmpl.find("{{") == std::string::npos)
        return tmpl;

    std::string out;
    out.reserve(tmpl.size());
    size_t pos = 0;
    while (pos < tmpl.size())
    {
        size_t open = tmpl.find("{{", pos);
        if (open == std::string::npos)
        {
            out.append(tmpl, pos, std::string::npos);
            break;
        }
        size_t close = tmpl.find("}}", open + 2);
        if (close == std::string::npos)
        {
            out.append(tmpl, pos, std::string::npos);
            break;
        }
        out.append(tmpl, pos, open - pos);
        std::string key = tmpl.substr(open + 2, close - open - 2);

        std::string value;
        if (key == "npc")        value = in.npcName;
        else if (key == "npc_entry") value = std::to_string(in.npcEntry);
        else if (key == "player")     value = in.playerName;
        else if (key == "target")     value = in.targetName;
        else if (key == "zone" || key == "map") value = in.zone;
        else if (key == "location")   value = in.location;
        else if (key == "time")       value = in.time;
        else if (key == "language")   value = in.language;
        else if (key == "last_line")  value = in.lastLine;
        else if (key == "bio")        value = bio;
        else if (key == "memories")   value = memories;
        else if (key == "summary")    value = summary;
        else if (key == "events")     value = events;
        // --- Spec 003: NPC vitals ---
        else if (key == "race")       value = in.race;
        else if (key == "gender")     value = in.gender;
        else if (key == "faction")    value = in.faction;
        else if (key == "subname")    value = in.subname;
        else if (key == "equipment")  value = in.equipment;
        else
        {
            // Unknown placeholder: leave it verbatim so the author can see the typo.
            if (g_warnedPlaceholders.insert(key).second)
                TC_LOG_WARN("AISystem", "AIPrompt: unknown placeholder '{{{{{}}}}}' left as-is.", key);
            out += "{{" + key + "}}";
            pos = close + 2;
            continue;
        }

        out += value;
        pos = close + 2;
    }
    return out;
}

std::string AIPrompt::CurrentTime()
{
    return CurrentTimeStr();
}

std::pair<std::string, std::string> AIPrompt::Build(AIPromptInput const& in,
                                                    std::string const& bio, std::string const& memories,
                                                    std::string const& summary, std::string const& events)
{
    std::string system, user;
    for (auto const& [file, role] : LoadSlots(in.type))
    {
        std::string resolved = Resolve(LoadSlot(in.type, file), in, bio, memories, summary, events);
        if (resolved.empty())
            continue;
        if (role == "system")
            system += system.empty() ? resolved : "\n\n" + resolved;
        else
            user += user.empty() ? resolved : "\n\n" + resolved;
    }
    if (system.empty())
        system = Resolve(BuiltinSystem(), in, bio, memories, summary, events);
    return { std::move(system), std::move(user) };
}

void AIPrompt::SeedDefaults()
{
    auto put = [](fs::path const& p, std::string const& content)
    {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        if (!Exists(p))
            WriteFile(p, content);
    };

    fs::path root = PromptRoot();
    put(root / "prompts/shared/system.md", BuiltinSystem());
    put(root / "prompts/shared/reinforcement.md", BuiltinReinforcement());
    // Spec 003: shared vitals block (overridable per-type).
    put(root / "prompts/shared/vitals.md",
        "[current vitals]\n"
        "- race/family: {{race}}\n"
        "- gender: {{gender}}\n"
        "- faction: {{faction}}\n"
        "- subname: {{subname}}\n"
        "[current equipment]\n"
        "{{equipment}}\n");

    char const* slotsText =
        "# slot-file : role     (role = system | user)\n"
        "system.md        : system\n"
        "bio.md           : user\n"
        "vitals.md        : user\n"
        "memories.md      : user\n"
        "summary.md       : user\n"
        "events.md        : user\n"
        "reinforcement.md : user\n";

    for (char const* type : { "player_chat", "npc_chat", "ambient" })
    {
        fs::path dir = root / "prompts" / type;
        put(dir / "slots.txt", slotsText);
        put(dir / "bio.md", "{{bio}}");
        put(dir / "vitals.md",
            "[current vitals]\n"
            "- race/family: {{race}}\n"
            "- gender: {{gender}}\n"
            "- faction: {{faction}}\n"
            "- subname: {{subname}}\n"
            "[current equipment]\n"
            "{{equipment}}\n");
        put(dir / "memories.md", "{{memories}}");
        put(dir / "summary.md", "Summary of previous conversation:\n{{summary}}");
        put(dir / "events.md", "Recent events:\n{{events}}");
        put(dir / "reinforcement.md", BuiltinReinforcement());

        if (std::string(type) == "player_chat")
        {
            put(dir / "system.md",
                "You are {{npc}}, a creature in the world of Warcraft. You are speaking with the "
                "player {{player}}.\nStay fully in character. Never mention that you are an AI or "
                "break character.\nRespond naturally and concisely (one or two sentences).\n"
                "Always reply in the language '{{language}}'.");
        }
        else if (std::string(type) == "npc_chat")
        {
            put(dir / "system.md",
                "You are {{npc}}, a creature in the world of Warcraft. Another creature "
                "({{target}}) is speaking near you.\nStay fully in character. Never mention that "
                "you are an AI or break character.\nRespond naturally and concisely (one or two "
                "sentences).\nAlways reply in the language '{{language}}'.");
        }
        else
        {
            put(dir / "system.md",
                "You are {{npc}}, a creature in the world of Warcraft.\nSay something in-character "
                "about your current surroundings or situation, as if thinking aloud. One or two "
                "sentences.\nNever mention that you are an AI or break character.\nAlways reply in "
                "the language '{{language}}'.");
        }
    }

    TC_LOG_INFO("AISystem", "AIPrompt: seeded default template tree under '{}'.", root.string());
}

void AIPrompt::IndexBios()
{
    g_bioIndex.clear();
    fs::path dir = PromptRoot() / "characters";
    if (!Exists(dir))
        return;
    std::error_code ec;
    for (auto const& entry : fs::directory_iterator(dir, ec))
    {
        if (!entry.is_regular_file(ec))
            continue;
        std::string fname = entry.path().filename().string();
        if (fname.size() < 4 || fname.substr(fname.size() - 3) != ".md")
            continue;
        // parse leading integer = creature entry
        size_t i = 0;
        uint64 val = 0;
        bool any = false;
        while (i < fname.size() && std::isdigit(static_cast<unsigned char>(fname[i])))
        {
            val = val * 10 + (fname[i] - '0');
            any = true;
            ++i;
        }
        if (any)
            g_bioIndex[uint32(val)] = entry.path().string();
    }
}

bool AIPrompt::Initialize()
{
    std::error_code ec;
    fs::create_directories(PromptRoot(), ec);
    if (!Exists(PromptRoot() / "prompts"))
        SeedDefaults();
    IndexBios();
    TC_LOG_INFO("AISystem", "AIPrompt: initialized (prompt root '{}', {} bios indexed).",
        PromptRoot().string(), g_bioIndex.size());
    return true;
}

void AIPrompt::Reload()
{
    g_cache.clear();
    g_warnedPlaceholders.clear();
    IndexBios();
    TC_LOG_INFO("AISystem", "AIPrompt: cache reloaded ({} templates cached, {} bios indexed).",
        g_cache.size(), g_bioIndex.size());
}

std::string AIPrompt::GetBio(uint32 entry, std::string const& name)
{
    // 1) Known from the index?
    auto it = g_bioIndex.find(entry);
    if (it != g_bioIndex.end())
    {
        std::string content = ReadCached(it->second);
        // Re-read even if unchanged is cheap (cache hit returns fast).
        return content;
    }

    // 2) Not indexed -> build the file name and auto-create an empty bio.
    std::string fname = std::to_string(entry) + " - " + SanitizeName(name.empty() ? "Unknown" : name) + ".md";
    fs::path dir = PromptRoot() / "characters";
    fs::path p = dir / fname;

    bool created = false;
    if (!Exists(p))
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream f(p);
        f << "# Bio — " << (name.empty() ? "Unknown" : name) << " (entry " << entry << ")\n\n"
          << "<!-- Describe this NPC here. This content is injected into {{bio}} for every "
             "prompt involving this NPC. -->\n";
        created = true;
        TC_LOG_INFO("AISystem", "AIPrompt: auto-created bio for entry {} ('{}').", entry, fname);
    }

    g_bioIndex[entry] = p.string();
    if (created)
        return ""; // freshly created empty bio
    return ReadCached(p.string());
}
