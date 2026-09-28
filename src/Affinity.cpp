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
//
// A character can pay to reroll a few times (Affinity.RerollCosts): the gold buys an offer, a
// different skill of the pool, and the character then takes it or keeps the one it has. The offer
// is stored with the affinity, so a logout or a crash before the choice does not lose it.

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
        REROLL_COSTS,

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
            SetConfigValue<std::string>(AffinityConfig::REROLL_COSTS, "Affinity.RerollCosts", "50,100,250");
        }
    };

    AffinityConfigData settings;

    // Affinity.RerollCosts in copper, one entry per reroll.
    std::vector<uint32> rerollCosts;

    void LoadRerollCosts()
    {
        rerollCosts.clear();
        for (std::string_view part : Acore::Tokenize(settings.GetConfigValue(AffinityConfig::REROLL_COSTS), ',', false))
            if (Optional<uint32> gold = Acore::StringTo<uint32>(part))
                rerollCosts.push_back(*gold * GOLD);
    }

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

    // The character's row of character_affinity, read at login. 0 = none yet / no offer pending.
    struct AffinityData : public DataMap::Base
    {
        uint32 poolId = 0;
        uint32 rerolls = 0;     // paid rerolls used
        uint32 offerId = 0;     // the reroll waiting for "take" or "keep"
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

    PoolEntry const* Offer(Player* player)
    {
        return Find(Data(player).offerId);
    }

    uint32 RerollsLeft(Player* player)
    {
        uint32 used = Data(player).rerolls;
        return used < rerollCosts.size() ? rerollCosts.size() - used : 0;
    }

    // Copper; 0 when no reroll is left.
    uint32 NextRerollCost(Player* player)
    {
        return RerollsLeft(player) ? rerollCosts[Data(player).rerolls] : 0;
    }

    std::string GoldText(uint32 copper)
    {
        return Acore::StringFormat("{}g", copper / GOLD);
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

    // The highest rank of the line the character knows: what the announcement links and the
    // addon shows, so the tooltip is the one of the spell actually being cast.
    uint32 KnownRank(Player const* player, uint32 spellId)
    {
        uint32 best = spellId;
        for (uint32 id = sSpellMgr->GetFirstSpellInChain(spellId); id; id = sSpellMgr->GetNextSpellInChain(id))
            if (player->HasSpell(id))
                best = id;
        return best;
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

    // Whispered in the addon language so the client raises CHAT_MSG_ADDON with the XorWoW prefix.
    void SendAddonMessage(Player* player, std::string const& body)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, Acore::StringFormat("{}\t{}", ADDON_PREFIX, body));
        player->SendDirectMessage(&data);
    }

    // "AFFINITY;NEW|CUR|SWAP;<pool id>;<spell id>;<text>;<skill names>;<rerolls left>;<next reroll
    // cost, copper>" or "AFFINITY;NONE". NEW is the discovery, SWAP a reroll taken, CUR the state.
    void SendToAddon(Player* player, PoolEntry const* entry, char const* state)
    {
        SendAddonMessage(player, entry
            ? Acore::StringFormat("AFFINITY;{};{};{};{};{};{};{}", state, entry->id, KnownRank(player, entry->spellId), entry->text, CoveredNames(*entry), RerollsLeft(player), NextRerollCost(player))
            : std::string("AFFINITY;NONE"));
    }

    // "AFFINITY;OFFER;<pool id>;<spell id>;<text>;<skill names>": a paid reroll waiting for a choice.
    void SendOffer(Player* player, PoolEntry const& entry)
    {
        SendAddonMessage(player, Acore::StringFormat("AFFINITY;OFFER;{};{};{};{}", entry.id, KnownRank(player, entry.spellId), entry.text, CoveredNames(entry)));
    }

    // "AFFINITY;ERR;<reason>": a reroll request refused, the reason in plain words.
    void SendError(Player* player, std::string const& reason)
    {
        SendAddonMessage(player, "AFFINITY;ERR;" + reason);
    }

    // The state at login and on the addon's request: the affinity, then the pending offer if any.
    void SendState(Player* player)
    {
        SendToAddon(player, Current(player), "CUR");
        if (PoolEntry const* offer = Offer(player))
            SendOffer(player, *offer);
    }

    char const* Possessive(Player* player)
    {
        return player->getGender() == GENDER_FEMALE ? "her" : "his";
    }

    // To everyone online and to the character's guild, as the config says.
    void Broadcast(Player* player, std::string const& message)
    {
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

    void Announce(Player* player, PoolEntry const& entry)
    {
        Broadcast(player, Acore::StringFormat("{} has reached level {} and discovered {} affinity for {}.",
            player->GetName(), player->GetLevel(), Possessive(player), SpellLink(KnownRank(player, entry.spellId))));
    }

    // Keeps the reroll count; a new affinity always clears the offer.
    void Save(Player* player, PoolEntry const& entry)
    {
        AffinityData& data = Data(player);
        data.poolId = entry.id;
        data.offerId = 0;
        CharacterDatabase.Execute("INSERT INTO `character_affinity` (`guid`, `pool_id`, `discovered_at`) VALUES ({}, {}, {}) "
            "ON DUPLICATE KEY UPDATE `pool_id` = VALUES(`pool_id`), `discovered_at` = VALUES(`discovered_at`), `offer_id` = 0",
            player->GetGUID().GetCounter(), entry.id, GameTime::GetGameTime().count());
    }

    // A random eligible entry other than the current affinity: the class's pool, narrowed to the
    // skills the character actually knows, weighted by the pool's weights. nullptr when nothing
    // qualified.
    PoolEntry const* Roll(Player* player)
    {
        PoolEntry const* current = Current(player);
        std::vector<PoolEntry const*> candidates;
        uint32 total = 0;
        for (PoolEntry const& candidate : pool)
            if (candidate.weight && &candidate != current && candidate.classId == player->getClass() && KnowsLine(player, candidate.spellId))
            {
                candidates.push_back(&candidate);
                total += candidate.weight;
            }

        if (candidates.empty())
            return nullptr;

        uint32 roll = urand(1, total);
        for (PoolEntry const* candidate : candidates)
        {
            if (roll <= candidate->weight)
                return candidate;
            roll -= candidate->weight;
        }
        return candidates.back();
    }

    // Puts the entry on the character, or a random eligible one (see Roll) when none is given.
    // Returns the entry or nullptr when nothing qualified.
    PoolEntry const* Grant(Player* player, PoolEntry const* entry, bool announce, char const* state = "NEW")
    {
        if (!entry)
            entry = Roll(player);
        if (!entry)
        {
            LOG_WARN("module", "mod-affinity: {} (class {}) knows no other skill of the pool, no affinity given", player->GetName(), player->getClass());
            return nullptr;
        }

        if (PoolEntry const* current = Current(player))
            Remove(player, *current);
        Save(player, *entry);
        Apply(player, *entry);
        if (announce)
            Announce(player, *entry);
        SendToAddon(player, entry, state);
        LOG_INFO("module", "mod-affinity: {} got affinity {} ({})", player->GetName(), entry->id, SpellName(entry->spellId));
        return entry;
    }

    // Paid rerolls. Each step returns the refusal in plain words, or an empty string.

    // Pays for the next reroll and stores a different affinity as the offer.
    std::string Reroll(Player* player)
    {
        AffinityData& data = Data(player);
        PoolEntry const* current = Current(player);
        if (!current)
            return "you have no affinity to reroll yet.";
        if (Offer(player))
            return "a reroll is already waiting for your choice.";
        if (!RerollsLeft(player))
            return "you have no rerolls left.";
        if (player->IsInCombat())
            return "not in combat.";

        PoolEntry const* offer = Roll(player);
        if (!offer)
            return "you know no other skill that could become your affinity.";

        uint32 cost = NextRerollCost(player);
        if (!player->HasEnoughMoney(cost))
            return Acore::StringFormat("a reroll costs {}.", GoldText(cost));

        player->ModifyMoney(-int32(cost));
        ++data.rerolls;
        data.offerId = offer->id;

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        player->SaveGoldToDB(trans);
        trans->Append(Acore::StringFormat("UPDATE `character_affinity` SET `rerolls` = {}, `offer_id` = {} WHERE `guid` = {}",
            data.rerolls, offer->id, player->GetGUID().GetCounter()).c_str());
        CharacterDatabase.CommitTransaction(trans);

        SendOffer(player, *offer);
        LOG_INFO("module", "mod-affinity: {} paid {} for reroll {}, offered {} ({}) against {} ({})", player->GetName(), GoldText(cost), data.rerolls,
            offer->id, SpellName(offer->spellId), current->id, SpellName(current->spellId));
        return "";
    }

    // Swaps the affinity for the offer.
    std::string Take(Player* player)
    {
        PoolEntry const* current = Current(player);
        PoolEntry const* offer = Offer(player);
        if (!current || !offer)
            return "no reroll is waiting for your choice.";
        if (player->IsInCombat())
            return "not in combat.";

        Broadcast(player, Acore::StringFormat("{} rerolled {} affinity and traded {} for {}.",
            player->GetName(), Possessive(player), SpellLink(KnownRank(player, current->spellId)), SpellLink(KnownRank(player, offer->spellId))));
        Grant(player, offer, false, "SWAP");
        return "";
    }

    // Keeps the affinity and drops the offer; the reroll stays spent.
    std::string Keep(Player* player)
    {
        PoolEntry const* current = Current(player);
        PoolEntry const* offer = Offer(player);
        if (!current || !offer)
            return "no reroll is waiting for your choice.";

        Broadcast(player, Acore::StringFormat("{} rerolled {} affinity, was offered {} and kept {}.",
            player->GetName(), Possessive(player), SpellLink(KnownRank(player, offer->spellId)), SpellLink(KnownRank(player, current->spellId))));
        Data(player).offerId = 0;
        CharacterDatabase.Execute("UPDATE `character_affinity` SET `offer_id` = 0 WHERE `guid` = {}", player->GetGUID().GetCounter());
        SendToAddon(player, current, "CUR");
        LOG_INFO("module", "mod-affinity: {} kept affinity {} over offer {}", player->GetName(), current->id, offer->id);
        return "";
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
            LoadRerollCosts();
        }

        void OnStartup() override
        {
            LoadPool();
        }
    };

    class AffinityPlayer : public PlayerScript
    {
    public:
        AffinityPlayer() : PlayerScript("AffinityPlayer", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_LEARN_SPELL, PLAYERHOOK_ON_UPDATE_ZONE, PLAYERHOOK_ON_DELETE_FROM_DB, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

        // The passive is not saved with the character's auras: put it back, or roll for a character
        // that was already past the level when the module arrived.
        void OnPlayerLogin(Player* player) override
        {
            AffinityData& data = Data(player);
            if (QueryResult result = CharacterDatabase.Query("SELECT `pool_id`, `rerolls`, `offer_id` FROM `character_affinity` WHERE `guid` = {}", player->GetGUID().GetCounter()))
            {
                data.poolId = (*result)[0].Get<uint32>();
                data.rerolls = (*result)[1].Get<uint8>();
                data.offerId = (*result)[2].Get<uint32>();
            }

            // An offer the pool no longer has (a retired entry): the reroll is given back.
            PoolEntry const* offer = Offer(player);
            if (data.offerId && (!offer || offer->classId != player->getClass()))
            {
                LOG_WARN("module", "mod-affinity: {}'s offer {} is gone from the pool, reroll given back", player->GetName(), data.offerId);
                data.offerId = 0;
                if (data.rerolls)
                    --data.rerolls;
                CharacterDatabase.Execute("UPDATE `character_affinity` SET `offer_id` = 0, `rerolls` = {} WHERE `guid` = {}", data.rerolls, player->GetGUID().GetCounter());
            }

            if (PoolEntry const* entry = Current(player))
            {
                Apply(player, *entry);
                SendState(player);
            }
            else
                Discover(player);
        }

        void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
        {
            Discover(player);
        }

        // A new rank of the boosted skill: the addon links and scans the rank actually cast.
        void OnPlayerLearnSpell(Player* player, uint32 spellId) override
        {
            PoolEntry const* entry = Current(player);
            if (entry && sSpellMgr->GetFirstSpellInChain(spellId) == sSpellMgr->GetFirstSpellInChain(entry->spellId))
                SendToAddon(player, entry, "CUR");
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

        // The addon whispers itself "XorWoW\t<request>"; answered here and swallowed. "AFFINITY?"
        // asks for the state (login, UI reload); "AFFINITY!REROLL", "AFFINITY!TAKE" and
        // "AFFINITY!KEEP" are the reroll buttons.
        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
        {
            if (lang != LANG_ADDON || type != CHAT_MSG_WHISPER || receiver != player)
                return true;

            std::string const prefix = Acore::StringFormat("{}\t", ADDON_PREFIX);
            if (msg.rfind(prefix, 0) != 0)
                return true;
            std::string_view request = std::string_view(msg).substr(prefix.size());

            std::string error;
            if (request == "AFFINITY?")
                SendState(player);
            else if (request == "AFFINITY!REROLL")
                error = Reroll(player);
            else if (request == "AFFINITY!TAKE")
                error = Take(player);
            else if (request == "AFFINITY!KEEP")
                error = Keep(player);
            else
                return true;

            if (!error.empty())
                SendError(player, error);
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
                { "reroll", HandleReroll, SEC_PLAYER,     Console::No },
                { "take",   HandleTake,   SEC_PLAYER,     Console::No },
                { "keep",   HandleKeep,   SEC_PLAYER,     Console::No },
                { "force",  HandleForce,  SEC_GAMEMASTER, Console::No },
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

        static std::string Describe(Player const* player, PoolEntry const& entry)
        {
            return Acore::StringFormat("{} - {} ({})", SpellLink(KnownRank(player, entry.spellId)), entry.text, entry.id);
        }

        static bool HandleShow(ChatHandler* handler, Optional<PlayerIdentifier> target)
        {
            Player* player = Target(handler, target);
            if (!player)
                return true;

            std::string who = player == handler->GetPlayer() ? "Your affinity" : Acore::StringFormat("{}'s affinity", player->GetName());
            if (PoolEntry const* entry = Current(player))
                handler->PSendSysMessage("{}: {}", who, Describe(player, *entry));
            else
                handler->PSendSysMessage("{}: none yet (discovered at level {}).", who, settings.GetConfigValue<uint32>(AffinityConfig::LEVEL));
            return true;
        }

        // ".affinity reroll" tells the price, ".affinity reroll confirm" pays it.
        static bool HandleReroll(ChatHandler* handler, Optional<std::string> confirm)
        {
            Player* player = handler->GetPlayer();
            PoolEntry const* current = Current(player);
            if (!confirm || *confirm != "confirm")
            {
                if (PoolEntry const* offer = Offer(player))
                    handler->PSendSysMessage("Your reroll offers {}: \".affinity take\" to swap, \".affinity keep\" to keep {}.",
                        SpellLink(KnownRank(player, offer->spellId)), SpellLink(KnownRank(player, current->spellId)));
                else if (!current)
                    handler->SendSysMessage("You have no affinity to reroll yet.");
                else if (!RerollsLeft(player))
                    handler->SendSysMessage("You have no rerolls left.");
                else
                    handler->PSendSysMessage("A reroll costs {} ({} left) and offers a different affinity; you then take it or keep {}. \".affinity reroll confirm\" to pay.",
                        GoldText(NextRerollCost(player)), RerollsLeft(player), SpellLink(KnownRank(player, current->spellId)));
                return true;
            }

            std::string error = Reroll(player);
            if (!error.empty())
            {
                handler->PSendSysMessage("Cannot reroll: {}", error);
                return true;
            }
            PoolEntry const* offer = Offer(player);
            handler->PSendSysMessage("Your reroll offers {} - {}: \".affinity take\" to swap, \".affinity keep\" to keep {}.",
                SpellLink(KnownRank(player, offer->spellId)), offer->text, SpellLink(KnownRank(player, current->spellId)));
            return true;
        }

        static bool HandleTake(ChatHandler* handler)
        {
            std::string error = Take(handler->GetPlayer());
            if (!error.empty())
                handler->PSendSysMessage("Cannot take the reroll: {}", error);
            return true;
        }

        static bool HandleKeep(ChatHandler* handler)
        {
            std::string error = Keep(handler->GetPlayer());
            if (!error.empty())
                handler->PSendSysMessage("Cannot keep: {}", error);
            return true;
        }

        // Free, announced, never the same skill; the paid rerolls are left as they are.
        static bool HandleForce(ChatHandler* handler, Optional<PlayerIdentifier> target)
        {
            Player* player = Target(handler, target);
            if (!player)
                return true;

            if (PoolEntry const* entry = Grant(player, nullptr, true))
                handler->PSendSysMessage("{} now has the affinity {}", player->GetName(), Describe(player, *entry));
            else
                handler->PSendSysMessage("{} knows no other skill of the pool for class {}.", player->GetName(), player->getClass());
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
            handler->PSendSysMessage("{} now has the affinity {}", player->GetName(), Describe(player, *entry));
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
            Data(player).rerolls = 0;
            Data(player).offerId = 0;
            SendToAddon(player, nullptr, "NONE");
            handler->PSendSysMessage("{}'s affinity and rerolls removed; the next level-up or login rolls a new one.", player->GetName());
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
