#!/usr/bin/env python3
"""Generates data/sql/db-world/base/mod_affinity_world.sql from the pool below and the client's Spell.dbc.

Every pool entry becomes one server-side passive spell (a `spell_dbc` row, id 250001+): a
SPELL_AURA_ADD_PCT_MODIFIER of +100% on the family bits of the boosted skill, exactly the shape of
the "Improved <skill>" talents (Improved Eviscerate = op 0, Improved Corruption = op 22, Improved
Power Word: Fortitude = op 8). The mask is what makes it precise, so the script checks every mask
against all spells of the same family and refuses overlaps inside a class.

    python tools/gen_affinity.py --dbc /path/to/Spell.dbc
    (docker cp ac-worldserver:/azerothcore/env/dist/data/dbc/Spell.dbc . gets the file)
"""
import argparse
import collections
import os
import struct
import sys

# Spell mod ops (SpellDefines.h)
DAMAGE = 0        # direct damage and heals: applied to the final amount, spell power included
ALL_EFFECTS = 8   # base points of every effect: buffs, absorbs
DOT = 22          # periodic damage and healing ticks

FIRST_ID = 250001

# (pool id, class, mask (A, B, C), ops, boosted spell (rank 1), extra spells covered by the same bits, text)
# The pool id is permanent: it is what character_affinity stores, so never renumber, only append.
POOL = [
    # Warrior (family 4)
    (1, 1, (0x10000, 0, 0), [ALL_EFFECTS], 6673, [], 'attack power doubled'),                     # Battle Shout
    (2, 1, (0, 0x80, 0), [ALL_EFFECTS], 469, [], 'health bonus doubled'),                          # Commanding Shout
    (3, 1, (0x20, 0, 0), [DOT], 772, [], 'bleed damage doubled'),                                  # Rend
    (4, 1, (0x80, 0, 0), [DAMAGE], 6343, [], 'damage doubled'),                                    # Thunder Clap
    (5, 1, (0x40, 0, 0), [DAMAGE], 78, [], 'damage doubled'),                                      # Heroic Strike
    (6, 1, (0x400000, 0, 0), [DAMAGE], 845, [], 'damage doubled'),                                 # Cleave
    (7, 1, (0x400, 0, 0), [DAMAGE], 6572, [], 'damage doubled'),                                   # Revenge
    (8, 1, (0, 0x100, 0), [DAMAGE], 34428, [], 'damage doubled'),                                  # Victory Rush
    (9, 1, (0x4, 0, 0), [DAMAGE], 7384, [], 'damage doubled'),                                     # Overpower
    (10, 1, (0x20000000, 0, 0), [DAMAGE], 5308, [], 'damage doubled'),                             # Execute
    (11, 1, (0x200000, 0, 0), [DAMAGE], 1464, [], 'damage doubled'),                               # Slam
    (12, 1, (0, 0x4, 0), [DAMAGE], 1680, [], 'damage doubled'),                                    # Whirlwind
    (13, 1, (0, 0x200, 0), [DAMAGE], 23922, [], 'damage doubled'),                                 # Shield Slam
    # Paladin (family 10)
    (14, 2, (0x80000000, 0, 0), [DAMAGE], 635, [], 'healing doubled'),                             # Holy Light
    (15, 2, (0x40000000, 0, 0), [DAMAGE], 19750, [], 'healing doubled'),                           # Flash of Light
    (16, 2, (0x2, 0, 0), [ALL_EFFECTS], 19740, [25782], 'attack power doubled'),                   # Blessing of Might (+Greater)
    (17, 2, (0x10000, 0, 0), [ALL_EFFECTS], 19742, [25894], 'mana regeneration doubled'),          # Blessing of Wisdom (+Greater)
    (18, 2, (0x1000000, 0, 0), [ALL_EFFECTS], 20217, [25898], 'stat bonus doubled'),               # Blessing of Kings (+Greater)
    (19, 2, (0x40, 0, 0), [ALL_EFFECTS], 465, [], 'armor doubled'),                                # Devotion Aura
    (20, 2, (0x8, 0, 0), [ALL_EFFECTS], 7294, [], 'damage doubled'),                               # Retribution Aura
    (21, 2, (0x20, 0, 0), [DOT], 26573, [], 'damage doubled'),                                     # Consecration
    (22, 2, (0, 0x2, 0), [DAMAGE], 879, [], 'damage doubled'),                                     # Exorcism
    (23, 2, (0, 0x80, 0), [DAMAGE], 24275, [], 'damage doubled'),                                  # Hammer of Wrath
    (24, 2, (0, 0x200000, 0), [DAMAGE], 2812, [], 'damage doubled'),                               # Holy Wrath
    # Hunter (family 9)
    (25, 3, (0x800, 0, 0), [DAMAGE], 3044, [], 'damage doubled'),                                  # Arcane Shot
    (26, 3, (0x1000, 0, 0), [DAMAGE], 2643, [], 'damage doubled'),                                 # Multi-Shot
    (27, 3, (0, 0x1, 0), [DAMAGE], 56641, [], 'damage doubled'),                                   # Steady Shot
    (28, 3, (0x4000, 0, 0), [DOT], 1978, [], 'damage doubled'),                                    # Serpent Sting
    (29, 3, (0, 0, 0x10000), [DAMAGE], 2973, [], 'damage doubled'),                                # Raptor Strike
    (30, 3, (0x800000, 0, 0), [DOT], 136, [], 'healing doubled'),                                  # Mend Pet
    (31, 3, (0x100000, 0, 0), [ALL_EFFECTS], 13165, [], 'ranged attack power doubled'),            # Aspect of the Hawk
    (32, 3, (0x400, 0, 0), [ALL_EFFECTS], 1130, [], 'attack power bonus doubled'),                 # Hunter's Mark
    (33, 3, (0x2000, 0, 0), [DAMAGE], 1510, [], 'damage doubled'),                                 # Volley
    (34, 3, (0x4, 0, 0), [DAMAGE, DOT], 13813, [], 'damage doubled'),                              # Explosive Trap (effect 13812)
    (35, 3, (0, 0, 0x20000), [DOT], 13795, [], 'damage doubled'),                                  # Immolation Trap (effect 13797)
    # Rogue (family 8)
    (36, 4, (0x2, 0, 0), [DAMAGE], 1752, [], 'damage doubled'),                                    # Sinister Strike
    (37, 4, (0x4, 0, 0), [DAMAGE], 53, [], 'damage doubled'),                                      # Backstab
    (38, 4, (0x200, 0, 0), [DAMAGE], 8676, [], 'damage doubled'),                                  # Ambush
    (39, 4, (0x20000, 0, 0), [DAMAGE], 2098, [], 'damage doubled'),                                # Eviscerate
    (40, 4, (0x100, 0, 0), [DOT], 703, [], 'bleed damage doubled'),                                # Garrote
    (41, 4, (0x100000, 0, 0), [DOT], 1943, [], 'bleed damage doubled'),                            # Rupture
    (42, 4, (0x40000, 0, 0), [ALL_EFFECTS], 5171, [], 'attack speed bonus doubled'),               # Slice and Dice
    (43, 4, (0x2000, 0, 0), [DAMAGE], 8679, [], 'poison damage doubled'),                          # Instant Poison
    (44, 4, (0x10000, 0, 0), [DOT], 2823, [], 'poison damage doubled'),                            # Deadly Poison
    # Priest (family 6)
    (45, 5, (0x8, 0, 0), [ALL_EFFECTS], 1243, [21562], 'stamina doubled'),                         # Power Word: Fortitude (+Prayer)
    (46, 5, (0x20, 0, 0), [ALL_EFFECTS], 14752, [27681], 'spirit doubled'),                        # Divine Spirit (+Prayer)
    (47, 5, (0x2, 0, 0), [ALL_EFFECTS], 588, [], 'armor and spell power doubled'),                 # Inner Fire
    (48, 5, (0x100, 0, 0), [ALL_EFFECTS], 976, [27683], 'shadow resistance doubled'),              # Shadow Protection (+Prayer)
    (49, 5, (0x1, 0, 0), [ALL_EFFECTS], 17, [], 'absorb doubled'),                                 # Power Word: Shield
    (50, 5, (0x40, 0, 0), [DOT], 139, [], 'healing doubled'),                                      # Renew
    (51, 5, (0x800, 0, 0), [DAMAGE], 2061, [], 'healing doubled'),                                 # Flash Heal
    (52, 5, (0x1000, 0, 0), [DAMAGE], 2060, [], 'healing doubled'),                                # Greater Heal
    (53, 5, (0x200, 0, 0), [DAMAGE], 596, [], 'healing doubled'),                                  # Prayer of Healing
    (54, 5, (0x80, 0, 0), [DAMAGE], 585, [], 'damage doubled'),                                    # Smite
    (55, 5, (0x100000, 0, 0), [DAMAGE, DOT], 14914, [], 'damage doubled'),                         # Holy Fire
    (56, 5, (0x2000, 0, 0), [DAMAGE], 8092, [], 'damage doubled'),                                 # Mind Blast
    (57, 5, (0x8000, 0, 0), [DOT], 589, [], 'damage doubled'),                                     # Shadow Word: Pain
    (58, 5, (0x2000000, 0, 0), [DOT], 2944, [], 'damage and healing doubled'),                     # Devouring Plague
    (59, 5, (0x8400000, 0, 0), [DAMAGE], 15237, [], 'damage and healing doubled'),                 # Holy Nova (heal 23455)
    # Death Knight (family 15)
    (60, 6, (0x2, 0, 0), [DAMAGE], 45477, [], 'damage doubled'),                                   # Icy Touch
    (61, 6, (0x1, 0, 0), [DAMAGE], 45462, [], 'damage doubled'),                                   # Plague Strike
    (62, 6, (0x400000, 0, 0), [DAMAGE], 45902, [], 'damage doubled'),                              # Blood Strike
    (63, 6, (0x10, 0, 0), [DAMAGE], 49998, [], 'damage doubled'),                                  # Death Strike
    (64, 6, (0x2000, 0, 0), [DAMAGE], 47541, [], 'damage and healing doubled'),                    # Death Coil
    (65, 6, (0x40000, 0, 0), [DAMAGE], 48721, [], 'damage doubled'),                               # Blood Boil
    (66, 6, (0x20, 0, 0x8), [DAMAGE], 43265, [], 'damage doubled'),                                # Death and Decay (tick 52212)
    (67, 6, (0, 0x20000000, 0), [DAMAGE], 56815, [], 'damage doubled'),                            # Rune Strike
    (68, 6, (0, 0x40000000, 0), [ALL_EFFECTS], 57330, [], 'strength and agility doubled'),         # Horn of Winter
    # Shaman (family 11)
    (69, 7, (0x1, 0, 0), [DAMAGE], 403, [], 'damage doubled'),                                     # Lightning Bolt
    (70, 7, (0x2, 0, 0), [DAMAGE], 421, [], 'damage doubled'),                                     # Chain Lightning
    (71, 7, (0x100000, 0, 0), [DAMAGE], 8042, [], 'damage doubled'),                               # Earth Shock
    (72, 7, (0x10000000, 0, 0), [DAMAGE, DOT], 8050, [], 'damage doubled'),                        # Flame Shock
    (73, 7, (0x80000000, 0, 0), [DAMAGE], 8056, [], 'damage doubled'),                             # Frost Shock
    (74, 7, (0x40, 0, 0), [DAMAGE], 331, [], 'healing doubled'),                                   # Healing Wave
    (75, 7, (0x80, 0, 0), [DAMAGE], 8004, [], 'healing doubled'),                                  # Lesser Healing Wave
    (76, 7, (0x100, 0, 0), [DAMAGE], 1064, [], 'healing doubled'),                                 # Chain Heal
    (77, 7, (0x400, 0, 0), [DAMAGE], 324, [], 'damage doubled'),                                   # Lightning Shield
    (78, 7, (0, 0x20, 0), [ALL_EFFECTS], 52127, [], 'mana returned doubled'),                      # Water Shield
    (79, 7, (0x40000000, 0, 0), [DAMAGE], 3599, [8190, 1535], 'fire totem damage doubled'),        # Searing Totem (+Magma, Fire Nova)
    (80, 7, (0x10000, 0, 0), [ALL_EFFECTS], 8075, [], 'strength and agility doubled'),             # Strength of Earth Totem (aura 8076)
    (81, 7, (0x8000, 0, 0), [ALL_EFFECTS], 8071, [], 'armor doubled'),                             # Stoneskin Totem (aura 8072)
    (82, 7, (0x4000, 0, 0), [ALL_EFFECTS], 5675, [], 'mana regeneration doubled'),                 # Mana Spring Totem (aura 5677)
    (83, 7, (0x2000000, 0, 0), [ALL_EFFECTS], 8227, [], 'spell power doubled'),                    # Flametongue Totem (aura 52109)
    # Mage (family 3)
    (84, 8, (0x1, 0, 0), [DAMAGE], 133, [], 'damage doubled'),                                     # Fireball
    (85, 8, (0x20, 0, 0), [DAMAGE], 116, [], 'damage doubled'),                                    # Frostbolt
    (86, 8, (0x2, 0, 0), [DAMAGE], 2136, [], 'damage doubled'),                                    # Fire Blast
    (87, 8, (0x10, 0, 0), [DAMAGE], 2948, [], 'damage doubled'),                                   # Scorch
    (88, 8, (0x200800, 0, 0), [DAMAGE], 5143, [], 'damage doubled'),                               # Arcane Missiles (missile 7268)
    (89, 8, (0x1000, 0, 0), [DAMAGE], 1449, [], 'damage doubled'),                                 # Arcane Explosion
    (90, 8, (0x4, 0, 0), [DAMAGE, DOT], 2120, [], 'damage doubled'),                               # Flamestrike
    (91, 8, (0x80, 0, 0), [DAMAGE], 10, [], 'damage doubled'),                                     # Blizzard
    (92, 8, (0x200, 0, 0), [DAMAGE], 120, [], 'damage doubled'),                                   # Cone of Cold
    (93, 8, (0x400, 0, 0), [ALL_EFFECTS], 1459, [23028], 'intellect doubled'),                     # Arcane Intellect (+Brilliance)
    (94, 8, (0x2000000, 0, 0), [ALL_EFFECTS], 7302, [168], 'armor and frost resistance doubled'),  # Ice Armor (+Frost Armor)
    (95, 8, (0x10000000, 0, 0), [ALL_EFFECTS], 6117, [], 'resistances and mana regeneration doubled'),  # Mage Armor
    (96, 8, (0x8, 0, 0), [ALL_EFFECTS], 543, [], 'absorb doubled'),                                # Fire Ward
    (97, 8, (0x100, 0, 0), [ALL_EFFECTS], 6143, [], 'absorb doubled'),                             # Frost Ward
    (98, 8, (0x8000, 0, 0), [ALL_EFFECTS], 1463, [], 'absorb doubled'),                            # Mana Shield
    # Warlock (family 5)
    (99, 9, (0x1, 0, 0), [DAMAGE], 686, [], 'damage doubled'),                                     # Shadow Bolt
    (100, 9, (0x4, 0, 0), [DAMAGE, DOT], 348, [], 'damage doubled'),                               # Immolate
    (101, 9, (0x2, 0, 0), [DOT], 172, [], 'damage doubled'),                                       # Corruption
    (102, 9, (0x400, 0, 0), [DOT], 980, [], 'damage doubled'),                                     # Curse of Agony
    (103, 9, (0, 0x2, 0), [DOT], 603, [], 'damage doubled'),                                       # Curse of Doom
    (104, 9, (0x8, 0, 0), [DOT], 689, [], 'drain doubled'),                                        # Drain Life
    (105, 9, (0x100, 0, 0), [DAMAGE], 5676, [], 'damage doubled'),                                 # Searing Pain
    (106, 9, (0, 0x80, 0), [DAMAGE], 6353, [], 'damage doubled'),                                  # Soul Fire
    (107, 9, (0x20, 0, 0), [DAMAGE], 5740, [], 'damage doubled'),                                  # Rain of Fire
    (108, 9, (0x40, 0, 0), [DAMAGE], 1949, [], 'damage doubled'),                                  # Hellfire
    (109, 9, (0x80000, 0, 0), [DAMAGE], 6789, [], 'damage and healing doubled'),                   # Death Coil
    (110, 9, (0, 0x20, 0), [ALL_EFFECTS], 706, [], 'armor and healing received doubled'),          # Demon Armor
    (111, 9, (0x1000000, 0, 0), [DOT], 755, [], 'pet healing doubled'),                            # Health Funnel
    # Druid (family 7)
    (112, 11, (0x40000, 0, 0), [ALL_EFFECTS], 1126, [21849], 'stat, armor and resistance bonus doubled'),  # Mark of the Wild (+Gift)
    (113, 11, (0x100, 0, 0), [ALL_EFFECTS], 467, [], 'damage doubled'),                            # Thorns
    (114, 11, (0x10, 0, 0), [DOT], 774, [], 'healing doubled'),                                    # Rejuvenation
    (115, 11, (0x40, 0, 0), [DAMAGE, DOT], 8936, [], 'healing doubled'),                           # Regrowth
    (116, 11, (0x20, 0, 0), [DAMAGE], 5185, [], 'healing doubled'),                                # Healing Touch
    (117, 11, (0x80, 0, 0), [DAMAGE], 740, [], 'healing doubled'),                                 # Tranquility
    (118, 11, (0x2, 0, 0), [DAMAGE, DOT], 8921, [], 'damage doubled'),                             # Moonfire
    (119, 11, (0x1, 0, 0), [DAMAGE], 5176, [], 'damage doubled'),                                  # Wrath
    (120, 11, (0x4, 0, 0), [DAMAGE], 2912, [], 'damage doubled'),                                  # Starfire
    (121, 11, (0x400000, 0, 0), [DAMAGE], 16914, [], 'damage doubled'),                            # Hurricane
    (122, 11, (0, 0, 0x40000), [DAMAGE], 1082, [], 'damage doubled'),                              # Claw
    (123, 11, (0x8000, 0, 0), [DAMAGE], 5221, [], 'damage doubled'),                               # Shred
    (124, 11, (0x1000, 0, 0), [DAMAGE, DOT], 1822, [], 'damage doubled'),                          # Rake
    (125, 11, (0, 0, 0x200000), [DOT], 1079, [], 'bleed damage doubled'),                          # Rip
    (126, 11, (0x10000, 0, 0), [DAMAGE], 6785, [], 'damage doubled'),                              # Ravage
    (127, 11, (0x800, 0, 0), [DAMAGE], 6807, [], 'damage doubled'),                                # Maul
    (128, 11, (0, 0x100000, 0), [DAMAGE], 779, [], 'damage doubled'),                              # Swipe (Bear)
    (129, 11, (0, 0x1000, 0), [ALL_EFFECTS], 29166, [], 'mana returned doubled'),                  # Innervate
]

# Spell.dbc 3.3.5a: 234 columns; the spell_dbc table has the same columns in the same order.
COLUMNS = (
    'ID Category DispelType Mechanic Attributes AttributesEx AttributesEx2 AttributesEx3 AttributesEx4 AttributesEx5 '
    'AttributesEx6 AttributesEx7 ShapeshiftMask unk_320_2 ShapeshiftExclude unk_320_3 Targets TargetCreatureType '
    'RequiresSpellFocus FacingCasterFlags CasterAuraState TargetAuraState ExcludeCasterAuraState ExcludeTargetAuraState '
    'CasterAuraSpell TargetAuraSpell ExcludeCasterAuraSpell ExcludeTargetAuraSpell CastingTimeIndex RecoveryTime '
    'CategoryRecoveryTime InterruptFlags AuraInterruptFlags ChannelInterruptFlags ProcTypeMask ProcChance ProcCharges '
    'MaxLevel BaseLevel SpellLevel DurationIndex PowerType ManaCost ManaCostPerLevel ManaPerSecond ManaPerSecondPerLevel '
    'RangeIndex Speed ModalNextSpell CumulativeAura Totem_1 Totem_2 Reagent_1 Reagent_2 Reagent_3 Reagent_4 Reagent_5 '
    'Reagent_6 Reagent_7 Reagent_8 ReagentCount_1 ReagentCount_2 ReagentCount_3 ReagentCount_4 ReagentCount_5 '
    'ReagentCount_6 ReagentCount_7 ReagentCount_8 EquippedItemClass EquippedItemSubclass EquippedItemInvTypes Effect_1 '
    'Effect_2 Effect_3 EffectDieSides_1 EffectDieSides_2 EffectDieSides_3 EffectRealPointsPerLevel_1 '
    'EffectRealPointsPerLevel_2 EffectRealPointsPerLevel_3 EffectBasePoints_1 EffectBasePoints_2 EffectBasePoints_3 '
    'EffectMechanic_1 EffectMechanic_2 EffectMechanic_3 ImplicitTargetA_1 ImplicitTargetA_2 ImplicitTargetA_3 '
    'ImplicitTargetB_1 ImplicitTargetB_2 ImplicitTargetB_3 EffectRadiusIndex_1 EffectRadiusIndex_2 EffectRadiusIndex_3 '
    'EffectAura_1 EffectAura_2 EffectAura_3 EffectAuraPeriod_1 EffectAuraPeriod_2 EffectAuraPeriod_3 '
    'EffectMultipleValue_1 EffectMultipleValue_2 EffectMultipleValue_3 EffectChainTargets_1 EffectChainTargets_2 '
    'EffectChainTargets_3 EffectItemType_1 EffectItemType_2 EffectItemType_3 EffectMiscValue_1 EffectMiscValue_2 '
    'EffectMiscValue_3 EffectMiscValueB_1 EffectMiscValueB_2 EffectMiscValueB_3 EffectTriggerSpell_1 '
    'EffectTriggerSpell_2 EffectTriggerSpell_3 EffectPointsPerCombo_1 EffectPointsPerCombo_2 EffectPointsPerCombo_3 '
    'EffectSpellClassMaskA_1 EffectSpellClassMaskA_2 EffectSpellClassMaskA_3 EffectSpellClassMaskB_1 '
    'EffectSpellClassMaskB_2 EffectSpellClassMaskB_3 EffectSpellClassMaskC_1 EffectSpellClassMaskC_2 '
    'EffectSpellClassMaskC_3 SpellVisualID_1 SpellVisualID_2 SpellIconID ActiveIconID SpellPriority '
    + ' '.join('Name_Lang_%s' % l for l in 'enUS enGB koKR frFR deDE enCN zhCN enTW zhTW esES esMX ruRU ptPT ptBR itIT Unk'.split()) + ' Name_Lang_Mask '
    + ' '.join('NameSubtext_Lang_%s' % l for l in 'enUS enGB koKR frFR deDE enCN zhCN enTW zhTW esES esMX ruRU ptPT ptBR itIT Unk'.split()) + ' NameSubtext_Lang_Mask '
    + ' '.join('Description_Lang_%s' % l for l in 'enUS enGB koKR frFR deDE enCN zhCN enTW zhTW esES esMX ruRU ptPT ptBR itIT Unk'.split()) + ' Description_Lang_Mask '
    + ' '.join('AuraDescription_Lang_%s' % l for l in 'enUS enGB koKR frFR deDE enCN zhCN enTW zhTW esES esMX ruRU ptPT ptBR itIT Unk'.split()) + ' AuraDescription_Lang_Mask '
    'ManaCostPct StartRecoveryCategory StartRecoveryTime MaxTargetLevel SpellClassSet SpellClassMask_1 SpellClassMask_2 '
    'SpellClassMask_3 MaxTargets DefenseType PreventionType StanceBarOrder EffectChainAmplitude_1 '
    'EffectChainAmplitude_2 EffectChainAmplitude_3 MinFactionID MinReputation RequiredAuraVision '
    'RequiredTotemCategoryID_1 RequiredTotemCategoryID_2 RequiredAreasID SchoolMask RuneCostID SpellMissileID '
    'PowerDisplayID EffectBonusMultiplier_1 EffectBonusMultiplier_2 EffectBonusMultiplier_3 SpellDescriptionVariableID '
    'SpellDifficultyID').split()
assert len(COLUMNS) == 234, len(COLUMNS)
COL = {name: i for i, name in enumerate(COLUMNS)}
FLOAT_COLS = {47, 77, 78, 79, 101, 102, 103, 119, 120, 121, 216, 217, 218, 229, 230, 231}
STRING_COLS = set(range(136, 152)) | set(range(153, 169)) | set(range(170, 186)) | set(range(187, 203))
TEMPLATE_SPELL = 14162   # Improved Eviscerate (Rank 1): the passive talent every row is modelled on


def read_dbc(path):
    with open(path, 'rb') as f:
        magic, records, fields, record_size, string_size = struct.unpack('<4sIIII', f.read(20))
        if magic != b'WDBC' or fields != 234:
            sys.exit('%s: not a 3.3.5a Spell.dbc (magic %r, %d fields)' % (path, magic, fields))
        data = f.read(records * record_size)
        strings = f.read(string_size)
    rows = {}
    for i in range(records):
        row = struct.unpack('<234I', data[i * record_size:(i + 1) * record_size])
        rows[row[0]] = row
    return rows, strings


def dbc_string(strings, offset):
    return strings[offset:strings.index(b'\0', offset)].decode('utf-8', 'replace')


def sql_string(s):
    return "'" + s.replace('\\', '\\\\').replace("'", "\\'") + "'"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dbc', required=True, help='Spell.dbc (3.3.5a), e.g. copied out of the ac-worldserver container')
    parser.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data', 'sql', 'db-world', 'base', 'mod_affinity_world.sql'))
    args = parser.parse_args()

    spells, strings = read_dbc(args.dbc)
    name_of = lambda sid: dbc_string(strings, spells[sid][COL['Name_Lang_enUS']])
    family_of = lambda sid: spells[sid][COL['SpellClassSet']]
    flags_of = lambda sid: spells[sid][COL['SpellClassMask_1']:COL['SpellClassMask_1'] + 3]

    by_family = collections.defaultdict(list)
    for sid, row in spells.items():
        if row[COL['SpellClassSet']]:
            by_family[row[COL['SpellClassSet']]].append(sid)

    template = spells[TEMPLATE_SPELL]
    errors, rows, pool_rows = [], [], []
    seen_ids = set()
    by_class = collections.defaultdict(list)
    for pool_id, class_id, mask, ops, spell_id, extra, text in POOL:
        if pool_id in seen_ids:
            errors.append('pool id %d used twice' % pool_id)
        seen_ids.add(pool_id)
        if spell_id not in spells:
            errors.append('%d: spell %d not in Spell.dbc' % (pool_id, spell_id))
            continue
        family = family_of(spell_id)
        if not family or not any(mask):
            errors.append('%d %s: no family or empty mask' % (pool_id, name_of(spell_id)))
            continue
        for other in by_class[class_id]:
            if other[1] == family and any(a & b for a, b in zip(other[2], mask)):
                errors.append('%d %s overlaps %d %s' % (pool_id, name_of(spell_id), other[0], name_of(other[3])))
        by_class[class_id].append((pool_id, family, mask, spell_id))

        # Who else is hit by these bits: informational, the family also holds NPC spells that can never use a player's mods.
        intended = {name_of(spell_id)} | {name_of(e) for e in extra}
        hits = collections.OrderedDict()
        for sid in by_family[family]:
            if any(a & b for a, b in zip(flags_of(sid), mask)):
                hits.setdefault(name_of(sid), []).append(sid)
        others = [n for n in hits if n not in intended]
        print('%3d %-28s %-18s ops=%-8s also: %s' % (pool_id, name_of(spell_id), ','.join('%x' % m for m in mask), ops, '; '.join(others[:6]) + (' ...' if len(others) > 6 else '') if others else '-'))

        row = list(template)
        aura_id = FIRST_ID + pool_id - 1
        row[COL['ID']] = aura_id
        row[COL['SpellIconID']] = spells[spell_id][COL['SpellIconID']]
        row[COL['SpellClassSet']] = family
        for i in range(3):
            row[COL['SpellClassMask_1'] + i] = 0
            row[COL['Effect_1'] + i] = 0
            row[COL['EffectAura_1'] + i] = 0
            row[COL['EffectDieSides_1'] + i] = 0
            row[COL['EffectBasePoints_1'] + i] = 0
            row[COL['EffectMiscValue_1'] + i] = 0
            row[COL['ImplicitTargetA_1'] + i] = 0
            row[COL['EffectSpellClassMaskA_1'] + i] = 0
            row[COL['EffectSpellClassMaskB_1'] + i] = 0
            row[COL['EffectSpellClassMaskC_1'] + i] = 0
        for i, op in enumerate(ops):
            row[COL['Effect_1'] + i] = 6            # SPELL_EFFECT_APPLY_AURA
            row[COL['EffectAura_1'] + i] = 108      # SPELL_AURA_ADD_PCT_MODIFIER
            row[COL['EffectBasePoints_1'] + i] = 100
            row[COL['EffectMiscValue_1'] + i] = op
            row[COL['ImplicitTargetA_1'] + i] = 1   # TARGET_UNIT_CASTER
            row[COL['EffectSpellClassMaskA_1'] + i] = mask[0]
            row[COL['EffectSpellClassMaskB_1'] + i] = mask[1]
            row[COL['EffectSpellClassMaskC_1'] + i] = mask[2]
        strings_out = {COL['Name_Lang_enUS']: 'Affinity: ' + name_of(spell_id), COL['Description_Lang_enUS']: name_of(spell_id) + ': ' + text}

        values = []
        for i, v in enumerate(row):
            if i in STRING_COLS:
                values.append(sql_string(strings_out.get(i, '')))
            elif i in FLOAT_COLS:
                values.append(repr(struct.unpack('<f', struct.pack('<I', v))[0]))
            else:
                values.append(str(struct.unpack('<i', struct.pack('<I', v))[0]))
        rows.append('(' + ', '.join(values) + ')')
        pool_rows.append('(%d, %d, %d, %d, %s, %s, %s)' % (pool_id, class_id, aura_id, spell_id, sql_string(','.join(str(e) for e in extra)), sql_string(text), sql_string(', '.join(sorted(intended)))))

    if errors:
        sys.exit('\n'.join(['ERROR: ' + e for e in errors]))

    last_id = FIRST_ID + max(p[0] for p in POOL) - 1
    with open(args.out, 'w', newline='\n') as f:
        f.write('-- Generated by tools/gen_affinity.py, do not edit: one server-side passive per affinity (+100%% spell mod on the\n'
                '-- boosted skill\'s family bits, modelled on the "Improved <skill>" talents) and the pool the module rolls from.\n\n')
        f.write('CREATE TABLE IF NOT EXISTS `affinity_pool` (\n'
                '  `id` INT UNSIGNED NOT NULL COMMENT \'permanent, stored in character_affinity\',\n'
                '  `class` TINYINT UNSIGNED NOT NULL,\n'
                '  `aura_id` INT UNSIGNED NOT NULL COMMENT \'spell_dbc passive applied to the character\',\n'
                '  `spell_id` INT UNSIGNED NOT NULL COMMENT \'boosted skill, rank 1: eligibility, icon, link\',\n'
                '  `extra_spells` VARCHAR(64) NOT NULL DEFAULT \'\' COMMENT \'other skills covered by the same bits\',\n'
                '  `text` VARCHAR(100) NOT NULL,\n'
                '  `weight` INT UNSIGNED NOT NULL DEFAULT 100 COMMENT \'relative roll chance, 0 removes the entry from the roll\',\n'
                '  `comment` VARCHAR(150) NOT NULL DEFAULT \'\',\n'
                '  PRIMARY KEY (`id`)\n'
                ') ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;\n\n')
        f.write('INSERT INTO `affinity_pool` (`id`, `class`, `aura_id`, `spell_id`, `extra_spells`, `text`, `comment`) VALUES\n')
        f.write(',\n'.join(pool_rows))
        f.write('\nON DUPLICATE KEY UPDATE `class` = VALUES(`class`), `aura_id` = VALUES(`aura_id`), `spell_id` = VALUES(`spell_id`), `extra_spells` = VALUES(`extra_spells`), `text` = VALUES(`text`), `comment` = VALUES(`comment`);\n\n')
        f.write('DELETE FROM `spell_dbc` WHERE `ID` BETWEEN %d AND %d;\n' % (FIRST_ID, last_id))
        f.write('INSERT INTO `spell_dbc` (%s) VALUES\n' % ', '.join('`%s`' % c for c in COLUMNS))
        f.write(',\n'.join(rows))
        f.write(';\n')
    print('%d affinities -> %s' % (len(rows), os.path.normpath(args.out)))


if __name__ == '__main__':
    main()
