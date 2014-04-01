/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>
#include <sys/types.h>

#include <gui/IDisplayEventConnection.h>
#include <gui/DisplayEventReceiver.h>

#include <utils/Errors.h>

#include "DisplayHardware/DisplayHardware.h"
#include "DisplayEventConnection.h"
#include "EventThread.h"
#include "SurfaceFlinger.h"

// ---------------------------------------------------------------------------

namespace android {

// ---------------------------------------------------------------------------

EventThread::EventThread(const sp<SurfaceFlinger>& flinger)
    : mFlinger(flinger),
      mHw(flinger->graphicPlane(0).displayHardware()),
      mLastVSyncTimestamp(0),
      mDeliveredEvents(0)
{
}

void EventThread::onFirstRef() {
    run("EventThread", PRIORITY_URGENT_DISPLAY + PRIORITY_MORE_FAVORABLE);
}

sp<DisplayEventConnection> EventThread::createEventConnection() const {
    return new DisplayEventConnection(const_cast<EventThread*>(this));
}

nsecs_t EventThread::getLastVSyncTimestamp() const {
    Mutex::Autolock _l(mLock);
    return mLastVSyncTimestamp;
}

nsecs_t EventThread::getVSyncPeriod() const {
    return mHw.getRefreshPeriod();

}

status_t EventThread::registerDisplayEventConnection(
        const sp<DisplayEventConnection>& connection) {
    Mutex::Autolock _l(mLock);
    ConnectionInfo info;
    mDisplayEventConnections.add(connection, info);
    mCondition.signal();
    return NO_ERROR;
}

status_t EventThread::unregisterDisplayEventConnection(
        const wp<DisplayEventConnection>& connection) {
    Mutex::Autolock _l(mLock);
    mDisplayEventConnections.removeItem(connection);
    mCondition.signal();
    return NO_ERROR;
}

void EventThread::removeDisplayEventConnection(
        const wp<DisplayEventConnection>& connection) {
    Mutex::Autolock _l(mLock);
    mDisplayEventConnections.removeItem(connection);
}

EventThread::ConnectionInfo* EventThread::getConnectionInfoLocked(
        const wp<DisplayEventConnection>& connection) {
    ssize_t index = mDisplayEventConnections.indexOfKey(connection);
    if (index < 0) return NULL;
    return &mDisplayEventConnections.editValueAt(index);
}

void EventThread::setVsyncRate(uint32_t count,
        const wp<DisplayEventConnection>& connection) {
    if (int32_t(count) >= 0) { // server must protect against bad params
        Mutex::Autolock _l(mLock);
        ConnectionInfo* info = getConnectionInfoLocked(connection);
        if (info) {
            const int32_t new_count = (count == 0) ? -1 : count;
            if (info->count != new_count) {
                info->count = new_count;
                mCondition.signal();
            }
        }
    }
}

void EventThread::requestNextVsync(
        const wp<DisplayEventConnection>& connection) {
    Mutex::Autolock _l(mLock);
    ConnectionInfo* info = getConnectionInfoLocked(connection);
    if (info && info->count < 0) {
        info->count = 0;
        mCondition.signal();
    }
}

bool EventThread::threadLoop() {

    nsecs_t timestamp;
    DisplayEventReceiver::Event vsync;
    Vector< wp<DisplayEventConnection> > displayEventConnections;

    { // scope for the lock
        Mutex::Autolock _l(mLock);
        do {
            // see if we need to wait for the VSYNC at all
            do {
                bool waitForNextVsync = false;
                size_t count = mDisplayEventConnections.size();
                for (size_t i=0 ; i<count ; i++) {
                    const ConnectionInfo& info(
                            mDisplayEventConnections.valueAt(i));
                    if (info.count >= 0) {
                        // at least one continuous mode or active one-shot event
                        waitForNextVsync = true;
                        break;
                    }
                }

                if (waitForNextVsync)
                    break;

                mCondition.wait(mLock);
            } while(true);

            // at least one listener requested VSYNC
            mLock.unlock();
            timestamp = mHw.waitForRefresh();
            mLock.lock();
            mDeliveredEvents++;
            mLastVSyncTimestamp = timestamp;

            // now see if we still need to report this VSYNC event
            const size_t count = mDisplayEventConnections.size();
            for (size_t i=0 ; i<count ; i++) {
                bool reportVsync = false;
                const ConnectionInfo& info(
                        mDisplayEventConnections.valueAt(i));
                if (info.count >= 1) {
                    if (info.count==1 || (mDeliveredEvents % info.count) == 0) {
                        // continuous event, and time to report it
                        reportVsync = true;
                    }
                } else if (info.count >= -1) {
                    ConnectionInfo& info(
                            mDisplayEventConnections.editValueAt(i));
                    if (info.count == 0) {
                        // fired this time around
                        reportVsync = true;
                    }
                    info.count--;
                }
                if (reportVsync) {
                    displayEventConnections.add(mDisplayEventConnections.keyAt(i));
                }
            }
        } while (!displayEventConnections.size());

        // dispatch vsync events to listeners...
        vsync.header.type = DisplayEventReceiver::DISPLAY_EVENT_VSYNC;
        vsync.header.timestamp = timestamp;
        vsync.vsync.count = mDeliveredEvents;
    }

    const size_t count = displayEventConnections.size();
    for (size_t i=0 ; i<count ; i++) {
        sp<DisplayEventConnection> conn(displayEventConnections[i].promote());
        // make sure the connection didn't die
        if (conn != NULL) {
            status_t err = conn->postEvent(vsync);
            if (err == -EAGAIN || err == -EWOULDBLOCK) {
                // The destination doesn't accept events anymore, it's probably
                // full. For now, we just drop the events on the floor.
                // Note that some events cannot be dropped and would have to be
                // re-sent later. Right-now we don't have the ability to do
                // this, but it doesn't matter for VSYNC.
            } else if (err < 0) {
                // handle any other error on the pipe as fatal. the only
                // reasonable thing to do is to clean-up this connection.
                // The most common error we'll get here is -EPIPE.
                removeDisplayEventConnection(displayEventConnections[i]);
            }
        } else {
            // somehow the connection is dead, but we still have it in our list
            // just clean the list.
            removeDisplayEventConnection(displayEventConnections[i]);
        }
    }

    // clear all our references without holding mLock
    displayEventConnections.clear();

    return true;
}

status_t EventThread::readyToRun() {
    ALOGI("EventThread ready to run.");
    return NO_ERROR;
}

void EventThread::dump(String8& result, char* buffer, size_t SIZE) const {
    Mutex::Autolock _l(mLock);
    result.append("VSYNC state:\n");
    snprintf(buffer, SIZE, "  numListeners=%u, events-delivered: %u\n",
            mDisplayEventConnections.size(), mDeliveredEvents);
    result.append(buffer);
}

// ---------------------------------------------------------------------------

}; // namespace android
