/****************************************************************************
Copyright (c) 2010-2012 cocos2d-x.org
Copyright (c) 2008-2010 Ricardo Quesada
Copyright (c) 2011      Zynga Inc.

http://www.cocos2d-x.org

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
****************************************************************************/

#include "CCScheduler.h"
#include "ccMacros.h"
#include "CCDirector.h"
#include "support/data_support/utlist.h"
#include "support/data_support/ccCArray.h"
#include "cocoa/CCArray.h"
#include "script_support/CCScriptSupport.h"

using namespace std;

NS_CC_BEGIN

// data structures

// A list double-linked list used for "updates with priority"
typedef struct _listEntry
{
    struct _listEntry   *prev, *next;
    Object            *target;        // not retained (retained by hashUpdateEntry)
    int                 priority;
    bool                paused;
    bool                markedForDeletion; // selector will no longer be called and entry will be removed at end of the next tick
} tListEntry;

typedef struct _hashUpdateEntry
{
    tListEntry          **list;        // Which list does it belong to ?
    tListEntry          *entry;        // entry in the list
    Object            *target;        // hash key (retained)
    UT_hash_handle      hh;
} tHashUpdateEntry;

// Hash Element used for "selectors with interval"
typedef struct _hashSelectorEntry
{
    ccArray             *timers;
    Object            *target;    // hash key (retained)
    unsigned int        timerIndex;
    Timer             *currentTimer;
    bool                currentTimerSalvaged;
    bool                paused;
    UT_hash_handle      hh;
} tHashTimerEntry;

// implementation Timer

Timer::Timer()
: _target(NULL)
, _elapsed(-1)
, _runForever(false)
, _useDelay(false)
, _timesExecuted(0)
, _repeat(0)
, _delay(0.0f)
, _interval(0.0f)
, _selector(NULL)
, _scriptHandler(0)
{
}

Timer* Timer::timerWithTarget(Object *pTarget, SEL_SCHEDULE pfnSelector)
{
    Timer *pTimer = new Timer();

    pTimer->initWithTarget(pTarget, pfnSelector, 0.0f, kRepeatForever, 0.0f);
    pTimer->autorelease();

    return pTimer;
}

Timer* Timer::timerWithTarget(Object *pTarget, SEL_SCHEDULE pfnSelector, float fSeconds)
{
    Timer *pTimer = new Timer();

    pTimer->initWithTarget(pTarget, pfnSelector, fSeconds, kRepeatForever, 0.0f);
    pTimer->autorelease();

    return pTimer;
}

Timer* Timer::timerWithScriptHandler(int nHandler, float fSeconds)
{
    Timer *pTimer = new Timer();

    pTimer->initWithScriptHandler(nHandler, fSeconds);
    pTimer->autorelease();

    return pTimer;
}

bool Timer::initWithScriptHandler(int nHandler, float fSeconds)
{
    _scriptHandler = nHandler;
    _elapsed = -1;
    _interval = fSeconds;

    return true;
}

bool Timer::initWithTarget(Object *pTarget, SEL_SCHEDULE pfnSelector)
{
    return initWithTarget(pTarget, pfnSelector, 0, kRepeatForever, 0.0f);
}

bool Timer::initWithTarget(Object *pTarget, SEL_SCHEDULE pfnSelector, float fSeconds, unsigned int nRepeat, float fDelay)
{
    _target = pTarget;
    _selector = pfnSelector;
    _elapsed = -1;
    _interval = fSeconds;
    _delay = fDelay;
    _useDelay = (fDelay > 0.0f) ? true : false;
    _repeat = nRepeat;
    _runForever = (nRepeat == kRepeatForever) ? true : false;
    return true;
}

void Timer::update(float dt)
{
    if (_elapsed == -1)
    {
        _elapsed = 0;
        _timesExecuted = 0;
    }
    else
    {
        if (_runForever && !_useDelay)
        {//standard timer usage
            _elapsed += dt;
            if (_elapsed >= _interval)
            {
                if (_target && _selector)
                {
                    (_target->*_selector)(_elapsed);
                }

                if (0 != _scriptHandler)
                {
                    SchedulerScriptData data(_scriptHandler,_elapsed);
                    ScriptEvent event(kScheduleEvent,&data);
                    ScriptEngineManager::sharedManager()->getScriptEngine()->sendEvent(&event);
                }
                _elapsed = 0;
            }
        }    
        else
        {//advanced usage
            _elapsed += dt;
            if (_useDelay)
            {
                if( _elapsed >= _delay )
                {
                    if (_target && _selector)
                    {
                        (_target->*_selector)(_elapsed);
                    }

                    if (0 != _scriptHandler)
                    {
                        SchedulerScriptData data(_scriptHandler,_elapsed);
                        ScriptEvent event(kScheduleEvent,&data);
                        ScriptEngineManager::sharedManager()->getScriptEngine()->sendEvent(&event);
                    }

                    _elapsed = _elapsed - _delay;
                    _timesExecuted += 1;
                    _useDelay = false;
                }
            }
            else
            {
                if (_elapsed >= _interval)
                {
                    if (_target && _selector)
                    {
                        (_target->*_selector)(_elapsed);
                    }

                    if (0 != _scriptHandler)
                    {
                        SchedulerScriptData data(_scriptHandler,_elapsed);
                        ScriptEvent event(kScheduleEvent,&data);
                        ScriptEngineManager::sharedManager()->getScriptEngine()->sendEvent(&event);
                    }

                    _elapsed = 0;
                    _timesExecuted += 1;

                }
            }

            if (!_runForever && _timesExecuted > _repeat)
            {    //unschedule timer
                Director::getInstance()->getScheduler()->unscheduleSelector(_selector, _target);
            }
        }
    }
}

float Timer::getInterval() const
{
    return _interval;
}

void Timer::setInterval(float fInterval)
{
    _interval = fInterval;
}

SEL_SCHEDULE Timer::getSelector() const
{
    return _selector;
}

// implementation of Scheduler

Scheduler::Scheduler(void)
: _timeScale(1.0f)
, _updatesNegList(NULL)
, _updates0List(NULL)
, _updatesPosList(NULL)
, _hashForUpdates(NULL)
, _hashForTimers(NULL)
, _currentTarget(NULL)
, _currentTargetSalvaged(false)
, _updateHashLocked(false)
, _scriptHandlerEntries(NULL)
{

}

Scheduler::~Scheduler(void)
{
    unscheduleAll();
    CC_SAFE_RELEASE(_scriptHandlerEntries);
}

void Scheduler::removeHashElement(_hashSelectorEntry *pElement)
{

	cocos2d::Object *target = pElement->target;

    ccArrayFree(pElement->timers);
    HASH_DEL(_hashForTimers, pElement);
    free(pElement);

    // make sure the target is released after we have removed the hash element
    // otherwise we access invalid memory when the release call deletes the target
    // and the target calls removeAllSelectors() during its destructor
    target->release();

}

void Scheduler::scheduleSelector(SEL_SCHEDULE pfnSelector, Object *pTarget, float fInterval, bool bPaused)
{
    this->scheduleSelector(pfnSelector, pTarget, fInterval, kRepeatForever, 0.0f, bPaused);
}

void Scheduler::scheduleSelector(SEL_SCHEDULE pfnSelector, Object *pTarget, float fInterval, unsigned int repeat, float delay, bool bPaused)
{
    CCAssert(pfnSelector, "Argument selector must be non-NULL");
    CCAssert(pTarget, "Argument target must be non-NULL");

    tHashTimerEntry *pElement = NULL;
    HASH_FIND_INT(_hashForTimers, &pTarget, pElement);

    if (! pElement)
    {
        pElement = (tHashTimerEntry *)calloc(sizeof(*pElement), 1);
        pElement->target = pTarget;
        if (pTarget)
        {
            pTarget->retain();
        }
        HASH_ADD_INT(_hashForTimers, target, pElement);

        // Is this the 1st element ? Then set the pause level to all the selectors of this target
        pElement->paused = bPaused;
    }
    else
    {
        CCAssert(pElement->paused == bPaused, "");
    }

    if (pElement->timers == NULL)
    {
        pElement->timers = ccArrayNew(10);
    }
    else 
    {
        for (unsigned int i = 0; i < pElement->timers->num; ++i)
        {
            Timer *timer = (Timer*)pElement->timers->arr[i];

            if (pfnSelector == timer->getSelector())
            {
                CCLOG("CCScheduler#scheduleSelector. Selector already scheduled. Updating interval from: %.4f to %.4f", timer->getInterval(), fInterval);
                timer->setInterval(fInterval);
                return;
            }        
        }
        ccArrayEnsureExtraCapacity(pElement->timers, 1);
    }

    Timer *pTimer = new Timer();
    pTimer->initWithTarget(pTarget, pfnSelector, fInterval, repeat, delay);
    ccArrayAppendObject(pElement->timers, pTimer);
    pTimer->release();    
}

void Scheduler::unscheduleSelector(SEL_SCHEDULE pfnSelector, Object *pTarget)
{
    // explicity handle nil arguments when removing an object
    if (pTarget == 0 || pfnSelector == 0)
    {
        return;
    }

    //CCAssert(pTarget);
    //CCAssert(pfnSelector);

    tHashTimerEntry *pElement = NULL;
    HASH_FIND_INT(_hashForTimers, &pTarget, pElement);

    if (pElement)
    {
        for (unsigned int i = 0; i < pElement->timers->num; ++i)
        {
            Timer *pTimer = (Timer*)(pElement->timers->arr[i]);

            if (pfnSelector == pTimer->getSelector())
            {
                if (pTimer == pElement->currentTimer && (! pElement->currentTimerSalvaged))
                {
                    pElement->currentTimer->retain();
                    pElement->currentTimerSalvaged = true;
                }

                ccArrayRemoveObjectAtIndex(pElement->timers, i, true);

                // update timerIndex in case we are in tick:, looping over the actions
                if (pElement->timerIndex >= i)
                {
                    pElement->timerIndex--;
                }

                if (pElement->timers->num == 0)
                {
                    if (_currentTarget == pElement)
                    {
                        _currentTargetSalvaged = true;
                    }
                    else
                    {
                        removeHashElement(pElement);
                    }
                }

                return;
            }
        }
    }
}

void Scheduler::priorityIn(tListEntry **ppList, Object *pTarget, int nPriority, bool bPaused)
{
    tListEntry *pListElement = (tListEntry *)malloc(sizeof(*pListElement));

    pListElement->target = pTarget;
    pListElement->priority = nPriority;
    pListElement->paused = bPaused;
    pListElement->next = pListElement->prev = NULL;
    pListElement->markedForDeletion = false;

    // empty list ?
    if (! *ppList)
    {
        DL_APPEND(*ppList, pListElement);
    }
    else
    {
        bool bAdded = false;

        for (tListEntry *pElement = *ppList; pElement; pElement = pElement->next)
        {
            if (nPriority < pElement->priority)
            {
                if (pElement == *ppList)
                {
                    DL_PREPEND(*ppList, pListElement);
                }
                else
                {
                    pListElement->next = pElement;
                    pListElement->prev = pElement->prev;

                    pElement->prev->next = pListElement;
                    pElement->prev = pListElement;
                }

                bAdded = true;
                break;
            }
        }

        // Not added? priority has the higher value. Append it.
        if (! bAdded)
        {
            DL_APPEND(*ppList, pListElement);
        }
    }

    // update hash entry for quick access
    tHashUpdateEntry *pHashElement = (tHashUpdateEntry *)calloc(sizeof(*pHashElement), 1);
    pHashElement->target = pTarget;
    pTarget->retain();
    pHashElement->list = ppList;
    pHashElement->entry = pListElement;
    HASH_ADD_INT(_hashForUpdates, target, pHashElement);
}

void Scheduler::appendIn(_listEntry **ppList, Object *pTarget, bool bPaused)
{
    tListEntry *pListElement = (tListEntry *)malloc(sizeof(*pListElement));

    pListElement->target = pTarget;
    pListElement->paused = bPaused;
    pListElement->markedForDeletion = false;

    DL_APPEND(*ppList, pListElement);

    // update hash entry for quicker access
    tHashUpdateEntry *pHashElement = (tHashUpdateEntry *)calloc(sizeof(*pHashElement), 1);
    pHashElement->target = pTarget;
    pTarget->retain();
    pHashElement->list = ppList;
    pHashElement->entry = pListElement;
    HASH_ADD_INT(_hashForUpdates, target, pHashElement);
}

void Scheduler::scheduleUpdateForTarget(Object *pTarget, int nPriority, bool bPaused)
{

    tHashUpdateEntry *pHashElement = NULL;
    HASH_FIND_INT(_hashForUpdates, &pTarget, pHashElement);
    if (pHashElement)
    {
#if COCOS2D_DEBUG >= 1
        CCAssert(pHashElement->entry->markedForDeletion,"");
#endif
        // TODO: check if priority has changed!

        pHashElement->entry->markedForDeletion = false;
        return;
    }

    // most of the updates are going to be 0, that's way there
    // is an special list for updates with priority 0
    if (nPriority == 0)
    {
        appendIn(&_updates0List, pTarget, bPaused);
    }
    else if (nPriority < 0)
    {
        priorityIn(&_updatesNegList, pTarget, nPriority, bPaused);
    }
    else
    {
        // priority > 0
        priorityIn(&_updatesPosList, pTarget, nPriority, bPaused);
    }
}

bool Scheduler::isScheduledForTarget(SEL_SCHEDULE pfnSelector, Object *pTarget)
{
    CCAssert(pfnSelector, "Argument selector must be non-NULL");
    CCAssert(pTarget, "Argument target must be non-NULL");
    
    tHashTimerEntry *pElement = NULL;
    HASH_FIND_INT(_hashForTimers, &pTarget, pElement);
    
    if (!pElement)
    {
        return false;
    }
    
    if (pElement->timers == NULL)
    {
        return false;
    }else
    {
        for (unsigned int i = 0; i < pElement->timers->num; ++i)
        {
            Timer *timer = (Timer*)pElement->timers->arr[i];
            
            if (pfnSelector == timer->getSelector())
            {
                return true;
            }
        }
        
        return false;
    }
    
    return false;  // should never get here
}

void Scheduler::removeUpdateFromHash(struct _listEntry *entry)
{
    tHashUpdateEntry *element = NULL;

    HASH_FIND_INT(_hashForUpdates, &entry->target, element);
    if (element)
    {
        // list entry
        DL_DELETE(*element->list, element->entry);
        free(element->entry);

        // hash entry
        Object* pTarget = element->target;
        HASH_DEL(_hashForUpdates, element);
        free(element);

        // target#release should be the last one to prevent
        // a possible double-free. eg: If the [target dealloc] might want to remove it itself from there
        pTarget->release();
    }
}

void Scheduler::unscheduleUpdateForTarget(const Object *pTarget)
{
    if (pTarget == NULL)
    {
        return;
    }

    tHashUpdateEntry *pElement = NULL;
    HASH_FIND_INT(_hashForUpdates, &pTarget, pElement);
    if (pElement)
    {
        if (_updateHashLocked)
        {
            pElement->entry->markedForDeletion = true;
        }
        else
        {
            this->removeUpdateFromHash(pElement->entry);
        }
    }
}

void Scheduler::unscheduleAll(void)
{
    unscheduleAllWithMinPriority(kPrioritySystem);
}

void Scheduler::unscheduleAllWithMinPriority(int nMinPriority)
{
    // Custom Selectors
    tHashTimerEntry *pElement = NULL;
    tHashTimerEntry *pNextElement = NULL;
    for (pElement = _hashForTimers; pElement != NULL;)
    {
        // pElement may be removed in unscheduleAllSelectorsForTarget
        pNextElement = (tHashTimerEntry *)pElement->hh.next;
        unscheduleAllForTarget(pElement->target);

        pElement = pNextElement;
    }

    // Updates selectors
    tListEntry *pEntry, *pTmp;
    if(nMinPriority < 0) 
    {
        DL_FOREACH_SAFE(_updatesNegList, pEntry, pTmp)
        {
            if(pEntry->priority >= nMinPriority) 
            {
                unscheduleUpdateForTarget(pEntry->target);
            }
        }
    }

    if(nMinPriority <= 0) 
    {
        DL_FOREACH_SAFE(_updates0List, pEntry, pTmp)
        {
            unscheduleUpdateForTarget(pEntry->target);
        }
    }

    DL_FOREACH_SAFE(_updatesPosList, pEntry, pTmp)
    {
        if(pEntry->priority >= nMinPriority) 
        {
            unscheduleUpdateForTarget(pEntry->target);
        }
    }

    if (_scriptHandlerEntries)
    {
        _scriptHandlerEntries->removeAllObjects();
    }
}

void Scheduler::unscheduleAllForTarget(Object *pTarget)
{
    // explicit NULL handling
    if (pTarget == NULL)
    {
        return;
    }

    // Custom Selectors
    tHashTimerEntry *pElement = NULL;
    HASH_FIND_INT(_hashForTimers, &pTarget, pElement);

    if (pElement)
    {
        if (ccArrayContainsObject(pElement->timers, pElement->currentTimer)
            && (! pElement->currentTimerSalvaged))
        {
            pElement->currentTimer->retain();
            pElement->currentTimerSalvaged = true;
        }
        ccArrayRemoveAllObjects(pElement->timers);

        if (_currentTarget == pElement)
        {
            _currentTargetSalvaged = true;
        }
        else
        {
            removeHashElement(pElement);
        }
    }

    // update selector
    unscheduleUpdateForTarget(pTarget);
}

unsigned int Scheduler::scheduleScriptFunc(unsigned int nHandler, float fInterval, bool bPaused)
{
    SchedulerScriptHandlerEntry* pEntry = SchedulerScriptHandlerEntry::create(nHandler, fInterval, bPaused);
    if (!_scriptHandlerEntries)
    {
        _scriptHandlerEntries = Array::createWithCapacity(20);
        _scriptHandlerEntries->retain();
    }
    _scriptHandlerEntries->addObject(pEntry);
    return pEntry->getEntryId();
}

void Scheduler::unscheduleScriptEntry(unsigned int uScheduleScriptEntryID)
{
    for (int i = _scriptHandlerEntries->count() - 1; i >= 0; i--)
    {
        SchedulerScriptHandlerEntry* pEntry = static_cast<SchedulerScriptHandlerEntry*>(_scriptHandlerEntries->objectAtIndex(i));
        if (pEntry->getEntryId() == (int)uScheduleScriptEntryID)
        {
            pEntry->markedForDeletion();
            break;
        }
    }
}

void Scheduler::resumeTarget(Object *pTarget)
{
    CCAssert(pTarget != NULL, "");

    // custom selectors
    tHashTimerEntry *pElement = NULL;
    HASH_FIND_INT(_hashForTimers, &pTarget, pElement);
    if (pElement)
    {
        pElement->paused = false;
    }

    // update selector
    tHashUpdateEntry *pElementUpdate = NULL;
    HASH_FIND_INT(_hashForUpdates, &pTarget, pElementUpdate);
    if (pElementUpdate)
    {
        CCAssert(pElementUpdate->entry != NULL, "");
        pElementUpdate->entry->paused = false;
    }
}

void Scheduler::pauseTarget(Object *pTarget)
{
    CCAssert(pTarget != NULL, "");

    // custom selectors
    tHashTimerEntry *pElement = NULL;
    HASH_FIND_INT(_hashForTimers, &pTarget, pElement);
    if (pElement)
    {
        pElement->paused = true;
    }

    // update selector
    tHashUpdateEntry *pElementUpdate = NULL;
    HASH_FIND_INT(_hashForUpdates, &pTarget, pElementUpdate);
    if (pElementUpdate)
    {
        CCAssert(pElementUpdate->entry != NULL, "");
        pElementUpdate->entry->paused = true;
    }
}

bool Scheduler::isTargetPaused(Object *pTarget)
{
    CCAssert( pTarget != NULL, "target must be non nil" );

    // Custom selectors
    tHashTimerEntry *pElement = NULL;
    HASH_FIND_INT(_hashForTimers, &pTarget, pElement);
    if( pElement )
    {
        return pElement->paused;
    }
    
    // We should check update selectors if target does not have custom selectors
	tHashUpdateEntry *elementUpdate = NULL;
	HASH_FIND_INT(_hashForUpdates, &pTarget, elementUpdate);
	if ( elementUpdate )
    {
		return elementUpdate->entry->paused;
    }
    
    return false;  // should never get here
}

Set* Scheduler::pauseAllTargets()
{
    return pauseAllTargetsWithMinPriority(kPrioritySystem);
}

Set* Scheduler::pauseAllTargetsWithMinPriority(int nMinPriority)
{
    Set* idsWithSelectors = new Set();// setWithCapacity:50];
    idsWithSelectors->autorelease();

    // Custom Selectors
    for(tHashTimerEntry *element = _hashForTimers; element != NULL;
        element = (tHashTimerEntry*)element->hh.next)
    {
        element->paused = true;
        idsWithSelectors->addObject(element->target);
    }

    // Updates selectors
    tListEntry *entry, *tmp;
    if(nMinPriority < 0) 
    {
        DL_FOREACH_SAFE( _updatesNegList, entry, tmp ) 
        {
            if(entry->priority >= nMinPriority) 
            {
                entry->paused = true;
                idsWithSelectors->addObject(entry->target);
            }
        }
    }

    if(nMinPriority <= 0) 
    {
        DL_FOREACH_SAFE( _updates0List, entry, tmp )
        {
            entry->paused = true;
            idsWithSelectors->addObject(entry->target);
        }
    }

    DL_FOREACH_SAFE( _updatesPosList, entry, tmp ) 
    {
        if(entry->priority >= nMinPriority) 
        {
            entry->paused = true;
            idsWithSelectors->addObject(entry->target);
        }
    }

    return idsWithSelectors;
}

void Scheduler::resumeTargets(Set* pTargetsToResume)
{
    SetIterator iter;
    for (iter = pTargetsToResume->begin(); iter != pTargetsToResume->end(); ++iter)
    {
        resumeTarget(*iter);
    }
}

// main loop
void Scheduler::update(float dt)
{
    _updateHashLocked = true;

    if (_timeScale != 1.0f)
    {
        dt *= _timeScale;
    }

    // Iterate over all the Updates' selectors
    tListEntry *pEntry, *pTmp;

    // updates with priority < 0
    DL_FOREACH_SAFE(_updatesNegList, pEntry, pTmp)
    {
        if ((! pEntry->paused) && (! pEntry->markedForDeletion))
        {
            pEntry->target->update(dt);
        }
    }

    // updates with priority == 0
    DL_FOREACH_SAFE(_updates0List, pEntry, pTmp)
    {
        if ((! pEntry->paused) && (! pEntry->markedForDeletion))
        {
            pEntry->target->update(dt);
        }
    }

    // updates with priority > 0
    DL_FOREACH_SAFE(_updatesPosList, pEntry, pTmp)
    {
        if ((! pEntry->paused) && (! pEntry->markedForDeletion))
        {
            pEntry->target->update(dt);
        }
    }

    // Iterate over all the custom selectors
    for (tHashTimerEntry *elt = _hashForTimers; elt != NULL; )
    {
        _currentTarget = elt;
        _currentTargetSalvaged = false;

        if (! _currentTarget->paused)
        {
            // The 'timers' array may change while inside this loop
            for (elt->timerIndex = 0; elt->timerIndex < elt->timers->num; ++(elt->timerIndex))
            {
                elt->currentTimer = (Timer*)(elt->timers->arr[elt->timerIndex]);
                elt->currentTimerSalvaged = false;

                elt->currentTimer->update(dt);

                if (elt->currentTimerSalvaged)
                {
                    // The currentTimer told the remove itself. To prevent the timer from
                    // accidentally deallocating itself before finishing its step, we retained
                    // it. Now that step is done, it's safe to release it.
                    elt->currentTimer->release();
                }

                elt->currentTimer = NULL;
            }
        }

        // elt, at this moment, is still valid
        // so it is safe to ask this here (issue #490)
        elt = (tHashTimerEntry *)elt->hh.next;

        // only delete currentTarget if no actions were scheduled during the cycle (issue #481)
        if (_currentTargetSalvaged && _currentTarget->timers->num == 0)
        {
            removeHashElement(_currentTarget);
        }
    }

    // Iterate over all the script callbacks
    if (_scriptHandlerEntries)
    {
        for (int i = _scriptHandlerEntries->count() - 1; i >= 0; i--)
        {
            SchedulerScriptHandlerEntry* pEntry = static_cast<SchedulerScriptHandlerEntry*>(_scriptHandlerEntries->objectAtIndex(i));
            if (pEntry->isMarkedForDeletion())
            {
                _scriptHandlerEntries->removeObjectAtIndex(i);
            }
            else if (!pEntry->isPaused())
            {
                pEntry->getTimer()->update(dt);
            }
        }
    }

    // delete all updates that are marked for deletion
    // updates with priority < 0
    DL_FOREACH_SAFE(_updatesNegList, pEntry, pTmp)
    {
        if (pEntry->markedForDeletion)
        {
            this->removeUpdateFromHash(pEntry);
        }
    }

    // updates with priority == 0
    DL_FOREACH_SAFE(_updates0List, pEntry, pTmp)
    {
        if (pEntry->markedForDeletion)
        {
            this->removeUpdateFromHash(pEntry);
        }
    }

    // updates with priority > 0
    DL_FOREACH_SAFE(_updatesPosList, pEntry, pTmp)
    {
        if (pEntry->markedForDeletion)
        {
            this->removeUpdateFromHash(pEntry);
        }
    }

    _updateHashLocked = false;

    _currentTarget = NULL;
}


NS_CC_END
