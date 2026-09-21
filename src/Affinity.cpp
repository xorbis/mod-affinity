/*
 * This file is part of mod-affinity, a module for AzerothCore, released under the
 * GNU GPL v2 license: https://github.com/xorbis/mod-affinity/blob/main/LICENSE
 */

// At level 60 a character discovers its affinity: one of the class skills it knows, drawn at random
// from the realm's pool (affinity_pool), whose effect is doubled for the rest of its life. The
// doubling is a hidden passive (a spell_dbc row per pool entry) carrying a +100% spell modifier on
// the skill's family bits, the same construct as the "Improved <skill>" talents, so the client's
// tooltips already show the doubled numbers. The choice is announced to the realm and the guild,
// stored in character_affinity, and handed to the XorWoW addon (spellbook entry, tooltip line,
// discovery banner) as an addon message.

#include "Chat.h"
#include "CommandScript.h"
#include "ConfigValueCache.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Guild.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#ifdef MOD_PLAYERBOTS
#include "RandomPlayerbotMgr.h"
#endif

#include <string>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    enum class AffinityConfig
    {
        ENABLE,
        LEVEL,
        ANNOUNCE,
        ANNOUNCE_GUILD,
        RANDOM_BOTS,

        NUM_CONFIGS
    };

    class AffinityConfigData : public ConfigValueCache<AffinityConfig>
    {
    public:
        AffinityConfigData() : ConfigValueCache(AffinityConfig::NUM_CONFIGS) { }

        void BuildConfigCache() override
        {
            SetConfigValue<bool>(AffinityConfig::ENABLE, "Affinity.Enable", true);
            SetConfigValue<uint32>(AffinityConfig::LEVEL, "Affinity.Level", 60,
                Reloadable::Yes, [](uint32 const& value) { return value >= 1 && value <= 80; }, "1-80");
            SetConfigValue<bool>(AffinityConfig::ANNOUNCE, "Affinity.Announce", true);
            SetConfigValue<bool>(AffinityConfig::ANNOUNCE_GUILD, "Affinity.AnnounceGuild", true);
            SetConfigValue<bool>(AffinityConfig::RANDOM_BOTS, "Affinity.RandomBots", false);
        }
    };

    AffinityConfigData settings;

    constexpr char ADDON_PREFIX[] = "XorWoW";

    // One row of affinity_pool.
    struct PoolEntry
    {
        uint32 id = 0;
        uint8 classId = 0;
        uint32 auraId = 0;                  // the hidden passive (spell_dbc)
        uint32 spellId = 0;                 // the boosted skill, rank 1: eligibility, icon, chat link
        std::vector<uint32> extraSpells;    // other skills the same bits cover (Greater Blessing of Might...)
        std::string text;                   // "damage doubled"
        uint32 weight = 100;
    };

    // Filled at startup and by ".affinity reload"; both run on the world thread, which never runs
    // alongside the map updates that read it.
    std::vector<PoolEntry> pool;

    // The character's choice, read from character_affinity at login. 0 = none yet.
    struct AffinityData : public DataMap::Base
    {
        uint32 poolId = 0;
    };

    constexpr char DATA_KEY[] = "mod-affinity";

    AffinityData& Data(Player* player)
    {
        return *player->CustomData.GetDefault<AffinityData>(DATA_KEY);
    }

    PoolEntry const* Find(uint32 poolId)
    {
        for (PoolEntry const& entry : pool)
            if (entry.id == poolId)
                return &entry;
        return nullptr;
    }

    PoolEntry const* Current(Player* player)
    {
        return Find(Data(player).poolId);
    }

    std::string SpellName(uint32 spellId)
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
            return info->SpellName[DEFAULT_LOCALE];
        return std::to_string(spellId);
    }

    std::string SpellLink(uint32 spellId)
    {
        return Acore::StringFormat("|cff71d5ff|Hspell:{}|h[{}]|h|r", spellId, SpellName(spellId));
    }

    // Every skill the entry covers, for the addon's tooltip line (ranks share their name).
    std::string CoveredNames(PoolEntry const& entry)
    {
        std::string names = SpellName(entry.spellId);
        for (uint32 spellId : entry.extraSpells)
            names += "," + SpellName(spellId);
        return names;
    }

    void LoadPool()
    {
        pool.clear();
        QueryResult result = WorldDatabase.Query("SELECT `id`, `class`, `aura_id`, `spell_id`, `extra_spells`, `text`, `weight` FROM `affinity_pool` ORDER BY `id`");
        if (!result)
        {
            LOG_ERROR("module", "mod-affinity: affinity_pool is empty, nobody can discover an affinity (is the module SQL applied?)");
            return;
        }

        do
        {
            Field* fields = result->Fetch();
            PoolEntry entry;
            entry.id = fields[0].Get<uint32>();
            entry.classId = fields[1].Get<uint8>();
            entry.auraId = fields[2].Get<uint32>();
            entry.spellId = fields[3].Get<uint32>();
            std::string extras = fields[4].Get<std::string>();
            for (std::string_view part : Acore::Tokenize(extras, ',', false))
                if (Optional<uint32> spellId = Acore::StringTo<uint32>(part))
                    entry.extraSpells.push_back(*spellId);
            entry.text = fields[5].Get<std::string>();
            entry.weight = fields[6].Get<uint32>();

            if (!sSpellMgr->GetSpellInfo(entry.auraId) || !sSpellMgr->GetSpellInfo(entry.spellId))
            {
                LOG_ERROR("module", "mod-affinity: pool entry {} skipped, spell {} or its passive {} does not exist", entry.id, entry.spellId, entry.auraId);
                continue;
            }
            pool.push_back(std::move(entry));
        } while (result->NextRow());

        LOG_INFO("module", "mod-affinity: {} affinities loaded", pool.size());
    }

    // The realm's random bots never get one (their announcements would flood the chat and they are
    // recycled all the time). A player's own characters played by the bot AI are players here.
    bool Eligible(Player* player)
    {
        if (!settings.GetConfigValue<bool>(AffinityConfig::ENABLE) || pool.empty())
            return false;
#ifdef MOD_PLAYERBOTS
        if (!settings.GetConfigValue<bool>(AffinityConfig::RANDOM_BOTS) && (sRandomPlayerbotMgr.IsRandomBot(player) || sRandomPlayerbotMgr.IsAddclassBot(player)))
            return false;
#endif
        return true;
    }

    // Any rank of the skill line counts; the higher ranks keep the lower ones in the spell list.
    bool KnowsLine(Player const* player, uint32 spellId)
    {
        for (uint32 id = sSpellMgr->GetFirstSpellInChain(spellId); id; id = sSpellMgr->GetNextSpellInChain(id))
            if (player->HasSpell(id))
                return true;
        return player->HasSpell(spellId);
    }

    void Apply(Player* player, PoolEntry const& entry)
    {
        if (!player->HasAura(entry.auraId))
            player->AddAura(entry.auraId, player);
    }

    void Remove(Player* player, PoolEntry const& entry)
    {
        player->RemoveAurasDueToSpell(entry.auraId);
    }

    // "AFFINITY;NEW|CUR;<pool id>;<spell id>;<text>;<skill names>" or "AFFINITY;NONE", whispered
    // in the addon language so the client raises CHAT_MSG_ADDON with the XorWoW prefix.
    void SendToAddon(Player* player, PoolEntry const* entry, bool fresh)
    {
        std::string body = entry
            ? Acore::StringFormat("AFFINITY;{};{};{};{};{}", fresh ? "NEW" : "CUR", entry->id, entry->spellId, entry->text, CoveredNames(*entry))
            : "AFFINITY;NONE";

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, Acore::StringFormat("{}\t{}", ADDON_PREFIX, body));
        player->SendDirectMessage(&data);
    }

    void Announce(Player* player, PoolEntry const& entry)
    {
        std::string message = Acore::StringFormat("{} has reached level {} and discovered {} affinity for {}.",
            player->GetName(), player->GetLevel(), player->getGender() == GENDER_FEMALE ? "her" : "his", SpellLink(entry.spellId));

        if (settings.GetConfigValue<bool>(AffinityConfig::ANNOUNCE))
        {
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_UNIVERSAL, nullptr, nullptr, message);
            sWorldSessionMgr->SendGlobalMessage(&data);
        }

        if (settings.GetConfigValue<bool>(AffinityConfig::ANNOUNCE_GUILD))
            if (Guild* guild = player->GetGuild())
            {
                WorldPacket data;
                ChatHandler::BuildChatPacket(data, CHAT_MSG_GUILD, LANG_UNIVERSAL, player, nullptr, message);
                guild->BroadcastPacket(&data);
            }
    }

    void Save(Player* player, PoolEntry const& entry)
    {
        Data(player).poolId = entry.id;
        CharacterDatabase.Execute("REPLACE INTO `character_affinity` (`guid`, `pool_id`, `discovered_at`) VALUES ({}, {}, {})",
            player->GetGUID().GetCounter(), entry.id, GameTime::GetGameTime().count());
    }

    // Puts the entry on the character, or a random eligible one when none is given: the class's
    // pool, narrowed to the skills the character actually knows, weighted by the pool's weights.
    // Returns the entry or nullptr when nothing qualified.
    PoolEntry const* Grant(Player* player, PoolEntry const* entry, bool announce)
    {
        if (!entry)
        {
            std::vector<PoolEntry const*> candidates;
            uint32 total = 0;
            for (PoolEntry const& candidate : pool)
                if (candidate.weight && candidate.classId == player->getClass() && KnowsLine(player, candidate.spellId))
                {
                    candidates.push_back(&candidate);
                    total += candidate.weight;
                }

            if (candidates.empty())
            {
                LOG_WARN("module", "mod-affinity: {} (class {}) knows none of the pool's skills, no affinity given", player->GetName(), player->getClass());
                return nullptr;
            }

            uint32 roll = urand(1, total);
            for (PoolEntry const* candidate : candidates)
            {
                if (roll <= candidate->weight)
                {
                    entry = candidate;
                    break;
                }
                roll -= candidate->weight;
            }
        }

        if (PoolEntry const* current = Current(player))
            Remove(player, *current);
        Save(player, *entry);
        Apply(player, *entry);
        if (announce)
            Announce(player, *entry);
        SendToAddon(player, entry, true);
        LOG_INFO("module", "mod-affinity: {} discovered affinity {} ({})", player->GetName(), entry->id, SpellName(entry->spellId));
        return entry;
    }

    void Discover(Player* player)
    {
        if (Eligible(player) && player->GetLevel() >= settings.GetConfigValue<uint32>(AffinityConfig::LEVEL) && !Current(player))
            Grant(player, nullptr, true);
    }

    class AffinityWorld : public WorldScript
    {
    public:
        AffinityWorld() : WorldScript("AffinityWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

        void OnAfterConfigLoad(bool reload) override
        {
            settings.Initialize(reload);
        }

        void OnStartup() override
        {
            LoadPool();
        }
    };

    class AffinityPlayer : public PlayerScript
    {
    public:
        AffinityPlayer() : PlayerScript("AffinityPlayer", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_UPDATE_ZONE, PLAYERHOOK_ON_DELETE_FROM_DB, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

        // The passive is not saved with the character's auras: put it back, or roll for a character
        // that was already past the level when the module arrived.
        void OnPlayerLogin(Player* player) override
        {
            AffinityData& data = Data(player);
            if (QueryResult result = CharacterDatabase.Query("SELECT `pool_id` FROM `character_affinity` WHERE `guid` = {}", player->GetGUID().GetCounter()))
                data.poolId = (*result)[0].Get<uint32>();

            if (PoolEntry const* entry = Current(player))
            {
                Apply(player, *entry);
                SendToAddon(player, entry, false);
            }
            else
                Discover(player);
        }

        void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
        {
            Discover(player);
        }

        // Cheap insurance against anything that strips every aura.
        void OnPlayerUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
        {
            if (PoolEntry const* entry = Current(player))
                Apply(player, *entry);
        }

        void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, ObjectGuid::LowType guid) override
        {
            trans->Append(Acore::StringFormat("DELETE FROM `character_affinity` WHERE `guid` = {}", guid).c_str());
        }

        // The addon asks for the current state at login and after a UI reload by whispering itself
        // "XorWoW\tAFFINITY?"; answered here and swallowed.
        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
        {
            if (lang != LANG_ADDON || type != CHAT_MSG_WHISPER || receiver != player)
                return true;
            if (msg.rfind(Acore::StringFormat("{}\tAFFINITY?", ADDON_PREFIX), 0) != 0)
                return true;

            SendToAddon(player, Current(player), false);
            return false;
        }
    };

    class AffinityCommands : public CommandScript
    {
    public:
        AffinityCommands() : CommandScript("AffinityCommands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable affinityCommandTable =
            {
                { "reroll", HandleReroll, SEC_GAMEMASTER, Console::No },
                { "set",    HandleSet,    SEC_GAMEMASTER, Console::No },
                { "reset",  HandleReset,  SEC_GAMEMASTER, Console::No },
                { "list",   HandleList,   SEC_GAMEMASTER, Console::Yes },
                { "reload", HandleReload, SEC_ADMINISTRATOR, Console::Yes },
                { "",       HandleShow,   SEC_PLAYER, Console::No },
            };
            static ChatCommandTable commandTable =
            {
                { "affinity", affinityCommandTable }
            };
            return commandTable;
        }

        static Player* Target(ChatHandler* handler, Optional<PlayerIdentifier> const& target)
        {
            Optional<PlayerIdentifier> who = target ? target : PlayerIdentifier::FromTargetOrSelf(handler);
            if (!who || !who->IsConnected())
            {
                handler->SendSysMessage("That character is not online.");
                return nullptr;
            }
            return who->GetConnectedPlayer();
        }

        static std::string Describe(PoolEntry const& entry)
        {
            return Acore::StringFormat("{} - {} ({})", SpellLink(entry.spellId), entry.text, entry.id);
        }

        static bool HandleShow(ChatHandler* handler, Optional<PlayerIdentifier> target)
        {
            Player* player = Target(handler, target);
            if (!player)
                return true;

            std::string who = player == handler->GetPlayer() ? "Your affinity" : Acore::StringFormat("{}'s affinity", player->GetName());
            if (PoolEntry const* entry = Current(player))
                handler->PSendSysMessage("{}: {}", who, Describe(*entry));
            else
                handler->PSendSysMessage("{}: none yet (discovered at level {}).", who, settings.GetConfigValue<uint32>(AffinityConfig::LEVEL));
            return true;
        }

        static bool HandleReroll(ChatHandler* handler, Optional<PlayerIdentifier> target)
        {
            Player* player = Target(handler, target);
            if (!player)
                return true;

            if (PoolEntry const* entry = Grant(player, nullptr, true))
                handler->PSendSysMessage("{} now has the affinity {}", player->GetName(), Describe(*entry));
            else
                handler->PSendSysMessage("{} knows none of the pool's skills for class {}.", player->GetName(), player->getClass());
            return true;
        }

        static bool HandleSet(ChatHandler* handler, uint32 poolId, Optional<PlayerIdentifier> target)
        {
            Player* player = Target(handler, target);
            if (!player)
                return true;

            PoolEntry const* entry = Find(poolId);
            if (!entry || entry->classId != player->getClass())
            {
                handler->PSendSysMessage("No pool entry {} for {}'s class; see .affinity list.", poolId, player->GetName());
                return true;
            }

            Grant(player, entry, false);
            handler->PSendSysMessage("{} now has the affinity {}", player->GetName(), Describe(*entry));
            return true;
        }

        static bool HandleReset(ChatHandler* handler, Optional<PlayerIdentifier> target)
        {
            Player* player = Target(handler, target);
            if (!player)
                return true;

            if (PoolEntry const* entry = Current(player))
                Remove(player, *entry);
            Data(player).poolId = 0;
            CharacterDatabase.Execute("DELETE FROM `character_affinity` WHERE `guid` = {}", player->GetGUID().GetCounter());
            SendToAddon(player, nullptr, false);
            handler->PSendSysMessage("{}'s affinity removed; the next level-up or login rolls a new one.", player->GetName());
            return true;
        }

        static bool HandleList(ChatHandler* handler, Optional<uint32> classId)
        {
            uint32 shown = 0;
            for (PoolEntry const& entry : pool)
                if (!classId || entry.classId == *classId)
                {
                    handler->PSendSysMessage("{:3}  class {:2}  {} - {}{}", entry.id, entry.classId, SpellName(entry.spellId), entry.text, entry.weight == 100 ? "" : Acore::StringFormat(" (weight {})", entry.weight));
                    ++shown;
                }
            handler->PSendSysMessage("{} affinities.", shown);
            return true;
        }

        static bool HandleReload(ChatHandler* handler)
        {
            LoadPool();
            handler->PSendSysMessage("affinity_pool reloaded: {} entries.", pool.size());
            return true;
        }
    };
}

void AddAffinityScripts()
{
    new AffinityWorld();
    new AffinityPlayer();
    new AffinityCommands();
}
