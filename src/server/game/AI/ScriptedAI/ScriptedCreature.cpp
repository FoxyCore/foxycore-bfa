/*
 * Copyright (C) 2021 BfaCore Reforged
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ScriptedCreature.h"
#include "AreaBoundary.h"
#include "DB2Stores.h"
#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "InstanceScript.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"

 // Spell summary for ScriptedAI::SelectSpell
struct TSpellSummary
{
    uint8 Targets;                                          // set of enum SelectTarget
    uint8 Effects;                                          // set of enum SelectEffect
} extern* SpellSummary;

void SummonList::Summon(Creature const* summon)
{
    storage_.push_back(summon->GetGUID());
}

void SummonList::Despawn(Creature const* summon)
{
    storage_.remove(summon->GetGUID());
}

void SummonList::DoZoneInCombat(uint32 entry, float maxRangeToNearestTarget)
{
    for (StorageType::iterator i = storage_.begin(); i != storage_.end();)
    {
        Creature* summon = ObjectAccessor::GetCreature(*me, *i);
        ++i;
        if (summon && summon->IsAIEnabled
                && (!entry || summon->GetEntry() == entry))
        {
            summon->AI()->DoZoneInCombat(nullptr, maxRangeToNearestTarget);
        }
    }
}

void SummonList::DespawnEntry(uint32 entry)
{
    for (StorageType::iterator i = storage_.begin(); i != storage_.end();)
    {
        Creature* summon = ObjectAccessor::GetCreature(*me, *i);
        if (!summon)
            i = storage_.erase(i);
        else if (summon->GetEntry() == entry)
        {
            i = storage_.erase(i);
            summon->DespawnOrUnsummon();
        }
        else
            ++i;
    }
}

void SummonList::DespawnAll()
{
    while (!storage_.empty())
    {
        Creature* summon = ObjectAccessor::GetCreature(*me, storage_.front());
        storage_.pop_front();
        if (summon)
            summon->DespawnOrUnsummon();
    }
}

void SummonList::RemoveNotExisting()
{
    for (StorageType::iterator i = storage_.begin(); i != storage_.end();)
    {
        if (ObjectAccessor::GetCreature(*me, *i))
            ++i;
        else
            i = storage_.erase(i);
    }
}

bool SummonList::HasEntry(uint32 entry) const
{
    for (StorageType::const_iterator i = storage_.begin(); i != storage_.end(); ++i)
    {
        Creature* summon = ObjectAccessor::GetCreature(*me, *i);
        if (summon && summon->GetEntry() == entry)
            return true;
    }

    return false;
}

void SummonList::DoActionImpl(int32 action, StorageType const& summons)
{
    for (auto const& guid : summons)
    {
        Creature* summon = ObjectAccessor::GetCreature(*me, guid);
        if (summon && summon->IsAIEnabled)
            summon->AI()->DoAction(action);
    }
}

ScriptedAI::ScriptedAI(Creature* creature) : CreatureAI(creature),
    IsFleeing(false),
	_checkHomeTimer(5000),
    summons(creature),
    damageEvents(creature),
    instance(creature->GetInstanceScript()),
    _isCombatMovementAllowed(true),
    IsLock(false),
    haseventdata(false),
    hastalkdata(false)
{
    _isHeroic = me->GetMap()->IsHeroic();
    _difficulty = me->GetMap()->GetDifficultyID();
}

void ScriptedAI::AttackStartNoMove(Unit* who)
{
    if (!who)
        return;

    if (me->Attack(who, true))
        DoStartNoMovement(who);
}

void ScriptedAI::AttackStart(Unit* who)
{
    if (IsCombatMovementAllowed())
        CreatureAI::AttackStart(who);
    else
        AttackStartNoMove(who);
}

void ScriptedAI::UpdateAI(uint32 diff)
{
    controls.Update(diff);
    //Check if we have a current target
    if (!UpdateVictim())
        return;

    events.Update(diff);

    if (me->HasUnitState(UNIT_STATE_CASTING))
        return;

    while (uint32 eventId = events.ExecuteEvent())
    {
        ExecuteEvent(eventId);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;
    }

    DoMeleeAttackIfReady();
}

void ScriptedAI::SetUnlock(uint32 time)
{
    me->GetScheduler().Schedule(Milliseconds(time), [this](TaskContext /*context*/)
    {
        IsLock = false;
    });
}

void ScriptedAI::DoStartMovement(Unit* victim, float distance, float angle)
{
    if (victim)
        me->GetMotionMaster()->MoveChase(victim, distance, angle);
}

void ScriptedAI::DoStartNoMovement(Unit* victim)
{
    if (!victim)
        return;

    me->GetMotionMaster()->MoveIdle();
}

void ScriptedAI::DoStopAttack()
{
    if (me->GetVictim())
        me->AttackStop();
}

void ScriptedAI::DoCastSpell(Unit* target, SpellInfo const* spellInfo, bool triggered)
{
    if (!target || me->IsNonMeleeSpellCast(false))
        return;

    me->StopMoving();
    me->CastSpell(target, spellInfo, triggered ? TRIGGERED_FULL_MASK : TRIGGERED_NONE);
}

void ScriptedAI::DoPlaySoundToSet(WorldObject* source, uint32 soundId)
{
    if (!source)
        return;

    if (!sSoundKitStore.LookupEntry(soundId))
    {
        TC_LOG_ERROR("scripts", "Invalid soundId %u used in DoPlaySoundToSet (Source: %s)", soundId, source->GetGUID().ToString().c_str());
        return;
    }

    source->PlayDirectSound(soundId);
}

Creature* ScriptedAI::DoSpawnCreature(uint32 entry, float offsetX, float offsetY, float offsetZ, float angle, uint32 type, uint32 despawntime)
{
    return me->SummonCreature(entry, me->GetPositionX() + offsetX, me->GetPositionY() + offsetY, me->GetPositionZ() + offsetZ, angle, TempSummonType(type), despawntime);
}

bool ScriptedAI::HealthBelowPct(uint32 pct) const
{
    return me->HealthBelowPct(pct);
}

bool ScriptedAI::HealthAbovePct(uint32 pct) const
{
    return me->HealthAbovePct(pct);
}

SpellInfo const* ScriptedAI::SelectSpell(Unit* target, uint32 school, uint32 mechanic, SelectTargetType targets, float rangeMin, float rangeMax, SelectEffect effect)
{
    //No target so we can't cast
    if (!target)
        return nullptr;

    //Silenced so we can't cast
    if (me->HasUnitFlag(UNIT_FLAG_SILENCED))
        return nullptr;

    //Using the extended script system we first create a list of viable spells
    SpellInfo const* apSpell[MAX_CREATURE_SPELLS];
    memset(apSpell, 0, MAX_CREATURE_SPELLS * sizeof(SpellInfo*));

    uint32 spellCount = 0;

    SpellInfo const* tempSpell = nullptr;

    //Check if each spell is viable(set it to null if not)
    for (uint32 i = 0; i < MAX_CREATURE_SPELLS; i++)
    {
        tempSpell = sSpellMgr->GetSpellInfo(me->m_spells[i]);

        //This spell doesn't exist
        if (!tempSpell)
            continue;

        // Targets and Effects checked first as most used restrictions
        //Check the spell targets if specified
        if (targets && !(SpellSummary[me->m_spells[i]].Targets & (1 << (targets-1))))
            continue;

        //Check the type of spell if we are looking for a specific spell type
        if (effect && !(SpellSummary[me->m_spells[i]].Effects & (1 << (effect-1))))
            continue;

        //Check for school if specified
        if (school && (tempSpell->SchoolMask & school) == 0)
            continue;

        //Check for spell mechanic if specified
        if (mechanic && tempSpell->Mechanic != mechanic)
            continue;

        //Check if the spell meets our range requirements
        if (rangeMin && me->GetSpellMinRangeForTarget(target, tempSpell) < rangeMin)
            continue;
        if (rangeMax && me->GetSpellMaxRangeForTarget(target, tempSpell) > rangeMax)
            continue;

        //Check if our target is in range
        if (me->IsWithinDistInMap(target, float(me->GetSpellMinRangeForTarget(target, tempSpell))) || !me->IsWithinDistInMap(target, float(me->GetSpellMaxRangeForTarget(target, tempSpell))))
            continue;

        //All good so lets add it to the spell list
        apSpell[spellCount] = tempSpell;
        ++spellCount;
    }

    //We got our usable spells so now lets randomly pick one
    if (!spellCount)
        return nullptr;

    return apSpell[urand(0, spellCount - 1)];
}

void ScriptedAI::DoResetThreat()
{
    if (!me->CanHaveThreatList() || me->getThreatManager().isThreatListEmpty())
    {
        TC_LOG_ERROR("scripts", "DoResetThreat called for creature that either cannot have threat list or has empty threat list (me entry = %d)", me->GetEntry());
        return;
    }

    ThreatContainer::StorageType threatlist = me->getThreatManager().getThreatList();

    for (ThreatContainer::StorageType::const_iterator itr = threatlist.begin(); itr != threatlist.end(); ++itr)
    {
        Unit* unit = ObjectAccessor::GetUnit(*me, (*itr)->getUnitGuid());
        if (unit && DoGetThreat(unit))
            DoModifyThreatPercent(unit, -100);
    }
}

float ScriptedAI::DoGetThreat(Unit* unit)
{
    if (!unit)
        return 0.0f;
    return me->getThreatManager().getThreat(unit);
}

void ScriptedAI::DoModifyThreatPercent(Unit* unit, int32 pct)
{
    if (!unit)
        return;
    me->getThreatManager().modifyThreatPercent(unit, pct);
}

void ScriptedAI::DoTeleportTo(float x, float y, float z, uint32 time)
{
    me->Relocate(x, y, z);
    float speed = me->GetDistance(x, y, z) / ((float)time * 0.001f);
    me->MonsterMoveWithSpeed(x, y, z, speed);
}

void ScriptedAI::DoTeleportTo(const float position[4])
{
    me->NearTeleportTo(position[0], position[1], position[2], position[3]);
}

void ScriptedAI::DoTeleportPlayer(Unit* unit, float x, float y, float z, float o)
{
    if (!unit)
        return;

    if (Player* player = unit->ToPlayer())
        player->TeleportTo(unit->GetMapId(), x, y, z, o, TELE_TO_NOT_LEAVE_COMBAT);
    else
        TC_LOG_ERROR("scripts", "Creature %s Tried to teleport non-player unit (%s) to x: %f y:%f z: %f o: %f. Aborted.",
            me->GetGUID().ToString().c_str(), unit->GetGUID().ToString().c_str(), x, y, z, o);
}

void ScriptedAI::DoTeleportAll(float x, float y, float z, float o)
{
    Map* map = me->GetMap();
    if (!map->IsDungeon())
        return;

    Map::PlayerList const& PlayerList = map->GetPlayers();
    for (Map::PlayerList::const_iterator itr = PlayerList.begin(); itr != PlayerList.end(); ++itr)
        if (Player* player = itr->GetSource())
            if (player->IsAlive())
                player->TeleportTo(me->GetMapId(), x, y, z, o, TELE_TO_NOT_LEAVE_COMBAT);
}

Unit* ScriptedAI::DoSelectLowestHpFriendly(float range, uint32 minHPDiff)
{
    Unit* unit = nullptr;
    Trinity::MostHPMissingInRange u_check(me, range, minHPDiff);
    Trinity::UnitLastSearcher<Trinity::MostHPMissingInRange> searcher(me, unit, u_check);
    Cell::VisitAllObjects(me, searcher, range);

    return unit;
}

Unit* ScriptedAI::DoSelectBelowHpPctFriendlyWithEntry(uint32 entry, float range, uint8 minHPDiff, bool excludeSelf)
{
    Unit* unit = nullptr;
    Trinity::FriendlyBelowHpPctEntryInRange u_check(me, entry, range, minHPDiff, excludeSelf);
    Trinity::UnitLastSearcher<Trinity::FriendlyBelowHpPctEntryInRange> searcher(me, unit, u_check);
    Cell::VisitAllObjects(me, searcher, range);

    return unit;
}

std::list<Creature*> ScriptedAI::DoFindFriendlyCC(float range)
{
    std::list<Creature*> list;
    Trinity::FriendlyCCedInRange u_check(me, range);
    Trinity::CreatureListSearcher<Trinity::FriendlyCCedInRange> searcher(me, list, u_check);
    Cell::VisitAllObjects(me, searcher, range);

    return list;
}

std::list<Creature*> ScriptedAI::DoFindFriendlyMissingBuff(float range, uint32 uiSpellid)
{
    std::list<Creature*> list;
    Trinity::FriendlyMissingBuffInRange u_check(me, range, uiSpellid);
    Trinity::CreatureListSearcher<Trinity::FriendlyMissingBuffInRange> searcher(me, list, u_check);
    Cell::VisitAllObjects(me, searcher, range);

    return list;
}

Player* ScriptedAI::GetPlayerAtMinimumRange(float minimumRange)
{
    Player* player = nullptr;

    Trinity::PlayerAtMinimumRangeAway check(me, minimumRange);
    Trinity::PlayerSearcher<Trinity::PlayerAtMinimumRangeAway> searcher(me, player, check);
    Cell::VisitWorldObjects(me, searcher, minimumRange);

    return player;
}

void ScriptedAI::SetEquipmentSlots(bool loadDefault, int32 mainHand /*= EQUIP_NO_CHANGE*/, int32 offHand /*= EQUIP_NO_CHANGE*/, int32 ranged /*= EQUIP_NO_CHANGE*/)
{
    if (loadDefault)
    {
        me->LoadEquipment(me->GetOriginalEquipmentId(), true);
        return;
    }

    if (mainHand >= 0)
        me->SetVirtualItem(0, uint32(mainHand));

    if (offHand >= 0)
        me->SetVirtualItem(1, uint32(offHand));

    if (ranged >= 0)
        me->SetVirtualItem(2, uint32(ranged));
}

void ScriptedAI::SetCombatMovement(bool allowMovement)
{
    _isCombatMovementAllowed = allowMovement;
}

bool ScriptedAI::CheckHomeDistToEvade(uint32 diff, float dist, float x, float y, float z, bool onlyZ)
{
    if (!me->IsInCombat())
        return false;

    bool evade = false;

    if (_checkHomeTimer <= diff)
    {
        _checkHomeTimer = 1500;

        if (onlyZ)
        {
            if ((me->GetPositionZ() > z + dist) || (me->GetPositionZ() < z - dist))
                evade = true;
        }
        else if (x != 0.0f || y != 0.0f || z != 0.0f)
        {
            if (me->GetDistance(x, y, z) >= dist)
                evade = true;
        }
        else if (me->GetDistance(me->GetHomePosition()) >= dist)
            evade = true;

        if (evade)
        {
            EnterEvadeMode();
            return true;
        }
    }
    else
    {
        _checkHomeTimer -= diff;
    }

    return false;
}

void ScriptedAI::LoadEventData(std::vector<EventData> const* data)
{
    eventList = data;
    haseventdata = true;
}

void ScriptedAI::GetEventData(uint16 group)
{
    if (!eventList)
        return;

    if (!haseventdata)
        return;

    for (EventData data : *eventList)
        if (data.group == group)
            events.ScheduleEvent(data.eventId, data.time, data.group, data.phase);
}

void ScriptedAI::LoadTalkData(std::vector<TalkData> const* data)
{
    if (data)
    {
        talkList = data;
        hastalkdata = true;
    }

}

void ScriptedAI::GetTalkData(uint32 eventId)
{
    if (!talkList)
        return;

    if (!hastalkdata)
        return;

    for (TalkData data : *talkList)
    {
        if (data.eventId == eventId)
        {
            switch (data.eventType)
            {
            case EVENT_TYPE_TALK:
                me->AI()->Talk(data.eventData);
                break;
            case EVENT_TYPE_CONVERSATION:
                if (data.eventData > 0)
                    if (instance)
                        instance->DoPlayConversation(data.eventData);
                break;
            case EVENT_TYPE_ACHIEVEMENT:
                if (data.eventData > 0)
                    if (instance)
                        instance->DoCompleteAchievement(data.eventData);
                break;
            case EVENT_TYPE_SPELL:
                if (data.eventData > 0)
                    if (instance)
                        instance->DoCastSpellOnPlayers(data.eventData);
                break;
            case EVENT_TYPE_YELL:
                if (data.eventData > 0)
                    me->Yell(data.eventData);
                break;
            case EVENT_TYPE_SAY:
                if (data.eventData > 0)
                    me->Say(data.eventData);
                break;
            }
        }
    }
}

void ScriptedAI::ApplyAllImmunities(bool apply)
{
    me->ApplySpellImmune(0, IMMUNITY_EFFECT, SPELL_EFFECT_KNOCK_BACK, apply);
    me->ApplySpellImmune(0, IMMUNITY_EFFECT, SPELL_EFFECT_KNOCK_BACK_DEST, apply);
    me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_GRIP, apply);
    me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_STUN, apply);
    me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_FEAR, apply);
    me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_ROOT, apply);
    me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_FREEZE, apply);
    me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_POLYMORPH, apply);
    me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_HORROR, apply);
    me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_SAPPED, apply);
    me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_CHARM, apply);
    me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_DISORIENTED, apply);
    me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_INTERRUPT, apply);
    me->ApplySpellImmune(0, IMMUNITY_STATE, SPELL_AURA_MOD_CONFUSE, apply);
}

void ScriptedAI::DespawnCreaturesInArea(uint32 entry, WorldObject* object)
{
    std::list<Creature*> creatures;
    GetCreatureListWithEntryInGrid(creatures, object, entry, 300.0f);
    if (creatures.empty())
        return;

    for (std::list<Creature*>::iterator itr = creatures.begin(); itr != creatures.end(); ++itr)
        (*itr)->DespawnOrUnsummon();
}

void ScriptedAI::DespawnGameObjectsInArea(uint32 entry, WorldObject* object)
{
    std::list<GameObject*> gameobjects;
    GetGameObjectListWithEntryInGrid(gameobjects, object, entry, 300.0f);
    if (gameobjects.empty())
        return;

    for (std::list<GameObject*>::iterator itr = gameobjects.begin(); itr != gameobjects.end();)
    {
        (*itr)->SetRespawnTime(0);
        (*itr)->Delete();
        itr = gameobjects.erase(itr);
    }
}

enum NPCs
{
    NPC_BROODLORD   = 12017,
    NPC_VOID_REAVER = 19516,
    NPC_JAN_ALAI    = 23578,
    NPC_SARTHARION  = 28860
};

void Scripted_NoMovementAI::AttackStart(Unit* target)
{
    if (!target)
        return;

    if (me->Attack(target, true))
        DoStartNoMovement(target);
}

// BossAI - for instanced bosses
BossAI::BossAI(Creature* creature, uint32 bossId) : ScriptedAI(creature),
    _bossId(bossId)
{
    if (instance)
        SetBoundary(instance->GetBossBoundary(bossId));
    _dungeonEncounterId = sObjectMgr->GetDungeonEncounterID(creature->GetEntry());
}

void BossAI::_Reset()
{
    if (!me->IsAlive())
        return;

    me->SetCombatPulseDelay(0);
    me->ResetLootMode();
    controls.Reset();
    events.Reset();
    summons.DespawnAll();
    me->RemoveAllAreaTriggers();
    me->GetScheduler().CancelAll();
    if (instance)
    {
        instance->SetBossState(_bossId, NOT_STARTED);
        instance->SendEncounterUnit(ENCOUNTER_FRAME_DISENGAGE, me);
        instance->SendEncounterUnit(ENCOUNTER_FRAME_INSTANCE_END, me);
        instance->SendEncounterUnit(ENCOUNTER_FRAME_UPDATE_ALLOWING_RELEASE, me, false);
        instance->SendEncounterUnit(ENCOUNTER_FRAME_UPDATE_SUPPRESSING_RELEASE, me, false);
    }
}

void BossAI::_JustDied()
{
    controls.Reset();
    events.Reset();
    summons.DespawnAll();
    me->GetScheduler().CancelAll();
    if (instance)
    {
        instance->SetBossState(_bossId, DONE);
        instance->SendEncounterUnit(ENCOUNTER_FRAME_DISENGAGE, me);
        instance->SendEncounterUnit(ENCOUNTER_FRAME_INSTANCE_END, me);
        instance->SendEncounterUnit(ENCOUNTER_FRAME_UPDATE_ALLOWING_RELEASE, me, false);
        instance->SendEncounterUnit(ENCOUNTER_FRAME_UPDATE_SUPPRESSING_RELEASE, me, false);
        if (_dungeonEncounterId > 0)
            instance->SendBossKillCredit(_dungeonEncounterId);
           instance->SetCheckPointPos(me->GetHomePosition());
    }
    Talk(BOSS_TALK_JUST_DIED);
    GetTalkData(EVENT_ON_JUSTDIED);
}

void BossAI::_JustReachedHome()
{
    me->setActive(false);

    if (instance)
        instance->SendEncounterUnit(ENCOUNTER_FRAME_DISENGAGE, me);
}

void BossAI::_KilledUnit(Unit* victim)
{
    if (victim->IsPlayer() && urand(0, 1))
        Talk(BOSS_TALK_KILL_PLAYER);
}

void BossAI::_DamageTaken(Unit* /*attacker*/, uint32& damage)
{
    while (uint32 eventId = damageEvents.OnDamageTaken(damage))
        ExecuteEvent(eventId);
}

void BossAI::_EnterCombat(bool showFrameEngage /*= true*/)
{
    if (instance)
    {
        // bosses do not respawn, check only on enter combat
        if (!instance->CheckRequiredBosses(_bossId))
        {
            EnterEvadeMode(EVADE_REASON_SEQUENCE_BREAK);
            return;
        }
        instance->SetBossState(_bossId, IN_PROGRESS);

        if (showFrameEngage) {

            instance->SendEncounterUnit(ENCOUNTER_FRAME_ENGAGE, me, 1);
            instance->SendEncounterUnit(ENCOUNTER_FRAME_INSTANCE_START, me);
            instance->SendEncounterUnit(ENCOUNTER_FRAME_UPDATE_ALLOWING_RELEASE, me, false);
            instance->SendEncounterUnit(ENCOUNTER_FRAME_UPDATE_SUPPRESSING_RELEASE, me, false);
        }
    }

    me->SetCombatPulseDelay(5);
    me->setActive(true);
    DoZoneInCombat();
    ScheduleTasks();
}

void BossAI::TeleportCheaters()
{
    float x, y, z;
    me->GetPosition(x, y, z);

    ThreatContainer::StorageType threatList = me->getThreatManager().getThreatList();
    for (ThreatContainer::StorageType::const_iterator itr = threatList.begin(); itr != threatList.end(); ++itr)
        if (Unit* target = (*itr)->getTarget())
            if (target->GetTypeId() == TYPEID_PLAYER && !CheckBoundary(target))
                target->NearTeleportTo(x, y, z, 0);
}

void BossAI::JustSummoned(Creature* summon)
{
    summons.Summon(summon);
    if (me->IsInCombat())
        DoZoneInCombat(summon);
}

void BossAI::SummonedCreatureDespawn(Creature* summon)
{
    summons.Despawn(summon);
}

void BossAI::UpdateAI(uint32 diff)
{
    controls.Update(diff);
    while (uint32 eventId = controls.ExecuteEvent())
        ExecuteEvent(eventId);

    if (!UpdateVictim())
        return;

    events.Update(diff);

    if (me->HasUnitState(UNIT_STATE_CASTING))
        return;

    while (uint32 eventId = events.ExecuteEvent())
    {
        ExecuteEvent(eventId);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;
    }

    DoMeleeAttackIfReady();
}

bool BossAI::CanAIAttack(Unit const* target) const
{
    return CheckBoundary(target);
}

void BossAI::_DespawnAtEvade(uint32 delayToRespawn, Creature* who)
{
    if (delayToRespawn < 2)
    {
        TC_LOG_ERROR("scripts", "_DespawnAtEvade called with delay of %u seconds, defaulting to 2.", delayToRespawn);
        delayToRespawn = 2;
    }

    if (!who)
        who = me;

    if (TempSummon* whoSummon = who->ToTempSummon())
    {
        TC_LOG_WARN("scripts", "_DespawnAtEvade called on a temporary summon.");
        whoSummon->UnSummon();
        return;
    }

    who->DespawnOrUnsummon(0, Seconds(delayToRespawn));

    if (instance && who == me)
        instance->SetBossState(_bossId, FAIL);
}

// StaticBossAI - for bosses that shouldn't move, and cast a spell when player are too far away

void StaticBossAI::_Reset()
{
    BossAI::_Reset();
    SetCombatMovement(false);
    _InitStaticSpellCast();
}

void StaticBossAI::_InitStaticSpellCast()
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(_staticSpell);
    if (!spellInfo)
        return;

    bool isAura = spellInfo->HasAura(me->GetMap()->GetDifficultyID());
    bool isArea = spellInfo->IsAffectingArea(me->GetMap()->GetDifficultyID());

    me->GetScheduler().Schedule(2s, [this, isAura, isArea](TaskContext context)
    {
        // Check if any player in range
        for (auto threat : me->getThreatManager().getThreatList())
        {
            if (me->IsWithinCombatRange(threat->getTarget(), me->GetCombatReach()))
            {
                me->RemoveAurasDueToSpell(_staticSpell);
                context.Repeat();
                return;
            }
        }

        // Else, cast spell depending of its effects
        if (isAura)
        {
            if (!me->HasAura(_staticSpell))
                me->CastSpell(me, _staticSpell, false);
        }
        else if (isArea)
        {
            me->CastSpell(nullptr, _staticSpell, false);
        }
        else
        {
            if (Unit* target = SelectTarget(SELECT_TARGET_RANDOM))
                me->CastSpell(target, _staticSpell, false);
        }

        context.Repeat();
    });
}

// WorldBossAI - for non-instanced bosses

WorldBossAI::WorldBossAI(Creature* creature) :
    ScriptedAI(creature) { }

void WorldBossAI::_Reset()
{
    if (!me->IsAlive())
        return;

    controls.Reset();
    events.Reset();
    summons.DespawnAll();
}

void WorldBossAI::_JustDied()
{
    controls.Reset();
    events.Reset();
    summons.DespawnAll();
}

void WorldBossAI::_EnterCombat()
{
    ScheduleTasks();
    Unit* target = SelectTarget(SELECT_TARGET_RANDOM, 0, 0.0f, true);
    if (target)
        AttackStart(target);
}

void WorldBossAI::JustSummoned(Creature* summon)
{
    summons.Summon(summon);
    Unit* target = SelectTarget(SELECT_TARGET_RANDOM, 0, 0.0f, true);
    if (target)
        summon->AI()->AttackStart(target);
}

void WorldBossAI::SummonedCreatureDespawn(Creature* summon)
{
    summons.Despawn(summon);
}

void GetPositionWithDistInOrientation(Position* pUnit, float dist, float orientation, float& x, float& y)
{
    x = pUnit->GetPositionX() + (dist * cos(orientation));
    y = pUnit->GetPositionY() + (dist * sin(orientation));
}

void GetPositionWithDistInOrientation(Position* fromPos, float dist, float orientation, Position& movePosition)
{
    float x = 0.0f;
    float y = 0.0f;

    GetPositionWithDistInOrientation(fromPos, dist, orientation, x, y);

    movePosition.m_positionX = x;
    movePosition.m_positionY = y;
    movePosition.m_positionZ = fromPos->GetPositionZ();
}

void GetRandPosFromCenterInDist(float centerX, float centerY, float dist, float& x, float& y)
{
    float randOrientation = frand(0.0f, 2.0f * (float)M_PI);

    x = centerX + (dist * cos(randOrientation));
    y = centerY + (dist * sin(randOrientation));
}

void GetRandPosFromCenterInDist(Position* centerPos, float dist, Position& movePosition)
{
    GetPositionWithDistInOrientation(centerPos, dist, frand(0, 2 * float(M_PI)), movePosition);
}

void GetPositionWithDistInFront(Position* centerPos, float dist, Position& movePosition)
{
    GetPositionWithDistInOrientation(centerPos, dist, centerPos->GetOrientation(), movePosition);
}
