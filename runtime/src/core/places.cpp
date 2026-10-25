#include "core/places.h"

#include <cstring>

namespace wf {
namespace {

constexpr const char* kSeaRooms[50] = {
    "the Great Sea",
    "Forsaken Fortress", "Star Island", "Northern Fairy Island", "Gale Isle", "Crescent Moon Island",
    "Seven-Star Isles", "Overlook Island", "Four-Eye Reef", "Mother and Child Isles", "Spectacle Island",
    "Windfall Island", "Pawprint Isle", "Dragon Roost Island", "Flight Control Platform", "Western Fairy Island",
    "Rock Spire Isle", "Tingle Island", "Northern Triangle Island", "Eastern Fairy Island", "Fire Mountain",
    "Star Belt Archipelago", "Three-Eye Reef", "Greatfish Isle", "Cyclops Reef", "Six-Eye Reef",
    "Tower of the Gods", "Eastern Triangle Island", "Thorned Fairy Island", "Needle Rock Isle", "Islet of Steel",
    "Stone Watcher Island", "Southern Triangle Island", "Private Oasis", "Bomb Island", "Bird's Peak Rock",
    "Diamond Steppe Island", "Five-Eye Reef", "Shark Island", "Southern Fairy Island", "Ice Ring Isle",
    "Forest Haven", "Cliff Plateau Isles", "Horseshoe Island", "Outset Island", "Headstone Island",
    "Two-Eye Reef", "Angular Isles", "Boating Course", "Five-Star Isles",
};

struct StageName { const char* prefix; const char* name; };
constexpr StageName kStages[] = {
    {"M_NewD2", "Dragon Roost Cavern"}, {"M_DragB", "Dragon Roost Cavern"}, {"kindan", "the Forbidden Woods"},
    {"kinBOSS", "the Forbidden Woods"}, {"Siren", "the Tower of the Gods"}, {"SirenB", "the Tower of the Gods"},
    {"M_Dai", "the Earth Temple"}, {"M_DaiB", "the Earth Temple"}, {"kaze", "the Wind Temple"}, {"kazeB", "the Wind Temple"},
    {"MajyuE", "the Forsaken Fortress"}, {"Mjtower", "the Forsaken Fortress"}, {"M2tower", "the Forsaken Fortress"},
    {"GanonK", "Ganon's Tower"}, {"GanonJ", "Ganon's Tower"}, {"GanonM", "Ganon's Tower"}, {"GTower", "Ganon's Tower"},
    {"Hyrule", "Hyrule"}, {"Hyroom", "Hyrule Castle"}, {"kenroom", "Hyrule Castle"},
    {"LinkRM", "Link's House"}, {"Ojhous", "Orca's House"}, {"Omasao", "Mesa's House"}, {"Atorizk", "Outset Island"},
    {"A_mori", "the Forest of Fairies"}, {"Pjavdou", "the Pirate Ship"}, {"PShip", "the Pirate Ship"},
    {"Obombh", "Windfall Island"}, {"Kaisen", "Windfall Island"}, {"Pnezumi", "Windfall Island"}, {"Opub", "Windfall Island"},
    {"Orichh", "Windfall Island"}, {"Ocrogh", "Windfall Island"}, {"Ekaze", "Windfall Island"}, {"Nitiyou", "Windfall Island"},
    {"Adanmae", "Dragon Roost Island"}, {"Comori", "Dragon Roost Island"},
    {"Omori", "the Forest Haven"}, {"Otkura", "the Forest Haven"}, {"ShipD", "the Great Sea"},
    {"TF_", "a submarine"}, {"SubD", "a submarine"}, {"Cave", "a cave"}, {"MiniKaz", "a cave"}, {"MiniHyo", "a cave"},
    {"ITest", "the test map"}, {"sea_T", "the title screen"}, {"Name", "the file select"},
};

}

std::string place_name(const char* stage, int room)
{
    char name[9] = {};
    std::strncpy(name, stage, 8);
    if (!name[0]) return {};
    if (std::strcmp(name, "sea") == 0) return room >= 0 && room < 50 ? kSeaRooms[room] : "the Great Sea";
    for (const StageName& s : kStages)
        if (std::strncmp(name, s.prefix, std::strlen(s.prefix)) == 0) return s.name;
    return {};
}

}
