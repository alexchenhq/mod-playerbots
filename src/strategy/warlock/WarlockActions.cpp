/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU GPL v2 license, you may redistribute it
 * and/or modify it under version 2 of the License, or (at your option), any later version.
 */

#include "WarlockActions.h"

#include <string>
#include <vector>
#include "Event.h"
#include "Item.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "Unit.h"
#include "Timer.h"
#include <unordered_map>
#include <mutex>

// Checks if the bot has less than 32 soul shards, and if so, allows casting Drain Soul
bool CastDrainSoulAction::isUseful() { return AI_VALUE2(uint32, "item count", "soul shard") < 32; }

// Checks if the bot's health is above a certain threshold, and if so, allows casting Life Tap
bool CastLifeTapAction::isUseful() { return AI_VALUE2(uint8, "health", "self target") > sPlayerbotAIConfig->lowHealth; }

// Checks if the target marked with the moon icon can be banished
bool CastBanishOnCcAction::isPossible()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    // Only possible on elementals or demons
    uint32 creatureType = target->GetCreatureType();
    if (creatureType != CREATURE_TYPE_DEMON && creatureType != CREATURE_TYPE_ELEMENTAL)
        return false;

    // Use base class to check spell available, range, etc
    return CastCrowdControlSpellAction::isPossible();
}

// Checks if the target marked with the moon icon can be feared
bool CastFearOnCcAction::isPossible()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    // Fear cannot be cast on mechanical or undead creatures
    uint32 creatureType = target->GetCreatureType();
    if (creatureType == CREATURE_TYPE_MECHANICAL || creatureType == CREATURE_TYPE_UNDEAD)
        return false;

    // Use base class to check spell available, range, etc
    return CastCrowdControlSpellAction::isPossible();
}

// Checks if the enemies are close enough to use Shadowflame
bool CastShadowflameAction::isUseful()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;
    bool facingTarget = AI_VALUE2(bool, "facing", "current target");
    bool targetClose = bot->IsWithinCombatRange(target, 7.0f);  // 7 yard cone
    return facingTarget && targetClose;
}

// Checks if the bot knows Seed of Corruption, and prevents the use of Rain of Fire if it does
bool CastRainOfFireAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target)
        return false;
    if (bot->HasSpell(27243) || bot->HasSpell(47835) || bot->HasSpell(47836)) // Seed of Corruption spell IDs
        return false;
    return true;
}

// Checks if the enemies are close enough to use Hellfire
bool CastHellfireAction::isUseful()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    return bot->IsWithinCombatRange(target, 5.0f);  // 5 yard AoE radius
}

// Checks if the "meta melee aoe" strategy is active, OR if the bot is in melee range of the target
bool CastImmolationAuraAction::isUseful()
{
    if (botAI->HasStrategy("meta melee", BOT_STATE_COMBAT))
        return true;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    if (!bot->HasAura(47241))  // 47241 is Metamorphosis spell ID (WotLK)
        return false;

    return bot->IsWithinCombatRange(target, 5.0f);  // 5 yard AoE radius
}

// Checks if the "warlock tank" strategy is active, and if so, prevents the use of Soulshatter
bool CastSoulshatterAction::isUseful()
{
    if (botAI->HasStrategy("tank", BOT_STATE_COMBAT))
        return false;
    return true;
}

// Checks if the target has a soulstone aura
static bool HasSoulstoneAura(Unit* unit)
{
    static const std::vector<uint32> soulstoneAuraIds = {20707, 20762, 20763, 20764, 20765, 27239, 47883};
    for (uint32 spellId : soulstoneAuraIds)
        if (unit->HasAura(spellId))
            return true;
    return false;
}

// Use the soulstone item on the bot itself with nc strategy "ss self"
bool UseSoulstoneSelfAction::Execute(Event event)
{
    std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", "soulstone");
    if (items.empty())
        return false;

    if (HasSoulstoneAura(bot))
        return false;

    bot->SetSelection(bot->GetGUID());
    return UseItem(items[0], ObjectGuid::Empty, nullptr, bot);
}

// Reservation map for soulstone targets (GUID -> reservation expiry in ms)
static std::unordered_map<ObjectGuid, uint32> soulstoneReservations;
static std::mutex soulstoneReservationsMutex;

// Helper to clean up expired reservations
void CleanupSoulstoneReservations()
{
    uint32 now = getMSTime();
    std::lock_guard<std::mutex> lock(soulstoneReservationsMutex);
    for (auto it = soulstoneReservations.begin(); it != soulstoneReservations.end();)
    {
        if (it->second <= now)
            it = soulstoneReservations.erase(it);
        else
            ++it;
    }
}

// Use the soulstone item on the bot's master with nc strategy "ss master"
bool UseSoulstoneMasterAction::Execute(Event event)
{
    CleanupSoulstoneReservations();

    std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", "soulstone");
    if (items.empty())
        return false;

    Player* master = botAI->GetMaster();
    if (!master || HasSoulstoneAura(master))
        return false;

    uint32 now = getMSTime();

    {
        std::lock_guard<std::mutex> lock(soulstoneReservationsMutex);
        if (soulstoneReservations.count(master->GetGUID()) && soulstoneReservations[master->GetGUID()] > now)
            return false;  // Already being soulstoned

        soulstoneReservations[master->GetGUID()] = now + 2500;  // Reserve for 2.5 seconds
    }

    float distance = sServerFacade->GetDistance2d(bot, master);
    if (distance >= 30.0f)
        return false;

    if (!bot->IsWithinLOSInMap(master))
        return false;

    bot->SetSelection(master->GetGUID());
    return UseItem(items[0], ObjectGuid::Empty, nullptr, master);
}

// Use the soulstone item on a tank in the group with nc strategy "ss tank"
bool UseSoulstoneTankAction::Execute(Event event)
{
    CleanupSoulstoneReservations();

    std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", "soulstone");
    if (items.empty())
        return false;

    Player* chosenTank = nullptr;
    Group* group = bot->GetGroup();
    uint32 now = getMSTime();

    // First: Try to soulstone the main tank
    if (group)
    {
        for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->GetSource();
            if (member && member->IsAlive() && botAI->IsTank(member) && botAI->IsMainTank(member) &&
                !HasSoulstoneAura(member))
            {
                std::lock_guard<std::mutex> lock(soulstoneReservationsMutex);
                if (soulstoneReservations.count(member->GetGUID()) && soulstoneReservations[member->GetGUID()] > now)
                    continue;  // Already being soulstoned

                float distance = sServerFacade->GetDistance2d(bot, member);
                if (distance < 30.0f && bot->IsWithinLOSInMap(member))
                {
                    chosenTank = member;
                    soulstoneReservations[chosenTank->GetGUID()] = now + 2500;  // Reserve for 2.5 seconds
                    break;                                                      
                }
            }
        }

        // If no main tank found, soulstone another tank
        if (!chosenTank)
        {
            for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
            {
                Player* member = gref->GetSource();
                if (member && member->IsAlive() && botAI->IsTank(member) && !HasSoulstoneAura(member))
                {
                    std::lock_guard<std::mutex> lock(soulstoneReservationsMutex);
                    if (soulstoneReservations.count(member->GetGUID()) &&
                        soulstoneReservations[member->GetGUID()] > now)
                        continue;  // Already being soulstoned

                    float distance = sServerFacade->GetDistance2d(bot, member);
                    if (distance < 30.0f && bot->IsWithinLOSInMap(member))
                    {
                        chosenTank = member;
                        soulstoneReservations[chosenTank->GetGUID()] = now + 2500;  // Reserve for 2.5 seconds
                        break;
                    }
                }
            }
        }
    }

    if (!chosenTank)
        return false;

    bot->SetSelection(chosenTank->GetGUID());
    return UseItem(items[0], ObjectGuid::Empty, nullptr, chosenTank);
}

// Use the soulstone item on a healer in the group with nc strategy "ss healer"
bool UseSoulstoneHealerAction::Execute(Event event)
{
    CleanupSoulstoneReservations();

    std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", "soulstone");
    if (items.empty())
        return false;

    Player* healer = nullptr;
    Group* group = bot->GetGroup();
    uint32 now = getMSTime();
    if (group)
    {
        for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->GetSource();
            if (member && member->IsAlive() && botAI->IsHeal(member) && !HasSoulstoneAura(member))
            {
                {
                    std::lock_guard<std::mutex> lock(soulstoneReservationsMutex);
                    if (soulstoneReservations.count(member->GetGUID()) &&
                        soulstoneReservations[member->GetGUID()] > now)
                        continue;  // Already being soulstoned

                    float distance = sServerFacade->GetDistance2d(bot, member);
                    if (distance < 30.0f && bot->IsWithinLOSInMap(member))
                    {
                        healer = member;
                        soulstoneReservations[healer->GetGUID()] = now + 2500;  // Reserve for 2.5 seconds
                        break;
                    }
                }
            }
        }
    }

    if (!healer)
        return false;

    bot->SetSelection(healer->GetGUID());
    return UseItem(items[0], ObjectGuid::Empty, nullptr, healer);
}
