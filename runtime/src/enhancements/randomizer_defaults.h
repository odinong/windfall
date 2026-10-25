#pragma once
namespace wf::rando {
inline constexpr const char* kDefaultConfig = R"YAML(program_version: cb7b1c9
file_version: 1.0
game_version: HD
seed: Windfall
progression_dungeons: Standard
progression_great_fairies: true
progression_puzzle_secret_caves: true
progression_combat_secret_caves: false
progression_short_sidequests: false
progression_long_sidequests: false
progression_spoils_trading: false
progression_minigames: false
progression_free_gifts: true
progression_mail: false
progression_platforms_rafts: false
progression_submarines: false
progression_eye_reef_chests: false
progression_big_octos_gunboats: false
progression_triforce_charts: false
progression_treasure_charts: false
progression_expensive_purchases: true
progression_misc: true
progression_tingle_chests: false
progression_battlesquid: false
progression_savage_labyrinth: false
progression_island_puzzles: false
progression_dungeon_secrets: false
progression_obscure: false
randomize_charts: false
randomize_starting_island: false
randomize_dungeon_entrances: false
randomize_boss_entrances: false
randomize_miniboss_entrances: false
randomize_cave_entrances: Disabled
randomize_door_entrances: false
randomize_misc_entrances: false
mix_dungeons: false
mix_bosses: false
mix_minibosses: false
mix_caves: false
mix_doors: false
mix_misc: false
decouple_entrances: false
ho_ho_hints: false
ho_ho_triforce_hints: false
korl_hints: false
korl_sword_hints: false
kreeb_bow_hints: false
clearer_hints: false
use_always_hints: false
hint_importance: false
path_hints: 0
barren_hints: 0
item_hints: 0
location_hints: 0
instant_text_boxes: true
fix_rng: false
performance: false
reveal_full_sea_chart: true
add_shortcut_warps_between_dungeons: false
skip_rematch_bosses: true
invert_sea_compass_x_axis: false
num_required_dungeons: 0
damage_multiplier: 2
chest_type_matches_contents: false
remove_swords: false
required_boss_items: false
starting_pohs: 0
starting_hcs: 3
starting_joy_pendants: 0
starting_skull_necklaces: 0
starting_boko_baba_seeds: 0
starting_golden_feathers: 0
starting_knights_crests: 0
starting_red_chu_jellys: 0
starting_green_chu_jellys: 0
starting_blue_chu_jellys: 0
remove_music: false
do_not_generate_spoiler_log: false
start_with_random_item: false
random_item_slide_item: false
classic_mode: false
plandomizer: false
open_drc: true
progressive_magic_always_double: false
dungeon_small_keys: Own Dungeon
dungeon_big_keys: Own Dungeon
dungeon_maps_compasses: Own Dungeon
starting_gear:
  - Progressive Sword
  - Progressive Shield
  - Ballad of Gales
  - Song of Passing
  - Progressive Magic Meter
  - Progressive Sail
excluded_locations:
  - Mailbox - Beedle's Gold Membership Reward
  - Mailbox - Beedle's Silver Membership Reward
  - Outset Island - Orca Hit 500 Times
)YAML";
inline constexpr const char* kDefaultPreferences = R"YAML(pig_color: Random
target_type: Hold
camera: Standard
first_person_camera: Standard
gyroscope: On
ui_display: On
quiet_swift_sail: false
player_in_casual_clothes: false
custom_player_model: Link
custom_colors:
  Pants: FFFFFF
  Hair: FFEF10
  Skin: F7DB9C
  Mouth: F74963
  Tunic: 5AB24A
  Undershirt: ADE342
  Eyes: 10514A
  Sclera: FFFFFF
  Boots: 944908
  Belt: 633008
  Belt Buckle: FFEF10
)YAML";
}
