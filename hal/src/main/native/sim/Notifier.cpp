/*----------------------------------------------------------------------------*/
/* Copyright (c) 2016-2020 FIRST. All Rights Reserved.                        */
/* Open Source Software - may be modified and shared by FRC teams. The code   */
/* must be accompanied by the FIRST BSD license file in the root directory of */
/* the project.                                                               */
/*----------------------------------------------------------------------------*/

#include "hal/Notifier.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include <wpi/SmallVector.h>
#include <wpi/condition_variable.h>
#include <wpi/mutex.h>

#include "HALInitializer.h"
#include "NotifierInternal.h"
#include "hal/Errors.h"
#include "hal/HALBase.h"
#include "hal/cpp/fpga_clock.h"
#include "hal/handles/UnlimitedHandleResource.h"
#include "hal/simulation/NotifierData.h"

namespace {
struct Notifier {
  std::string name;
  uint64_t waitTime;
  bool active = true;
  bool running = false;
  uint64_t count = 0;
  wpi::mutex mutex;
  wpi::condition_variable cond;
};
}  // namespace

using namespace hal;

static wpi::mutex notifiersWaiterMutex;
static wpi::condition_variable notifiersWaiterCond;

class NotifierHandleContainer
    : public UnlimitedHandleResource<HAL_NotifierHandle, Notifier,
                                     HAL_HandleEnum::Notifier> {
 public:
  ~NotifierHandleContainer() {
    ForEach([](HAL_NotifierHandle handle, Notifier* notifier) {
      {
        std::scoped_lock lock(notifier->mutex);
        notifier->active = false;
        notifier->running = false;
      }
      notifier->cond.notify_all();  // wake up any waiting threads
    });
    notifiersWaiterCond.notify_all();
  }
};

static NotifierHandleContainer* notifierHandles;
static std::atomic<bool> notifiersPaused{false};

namespace hal {
namespace init {
void InitializeNotifier() {
  static NotifierHandleContainer nH;
  notifierHandles = &nH;
}
}  // namespace init

void PauseNotifiers() { notifiersPaused = true; }

void ResumeNotifiers() {
  notifiersPaused = false;
  WakeupNotifiers();
}

void WakeupNotifiers() {
  notifierHandles->ForEach([](HAL_NotifierHandle handle, Notifier* notifier) {
    notifier->cond.notify_all();
  });
}

void WakeupWaitNotifiers() {
  std::unique_lock ulock(notifiersWaiterMutex);
  int32_t status = 0;
  uint64_t curTime = HAL_GetFPGATime(&status);
  wpi::SmallVector<std::pair<HAL_NotifierHandle, uint64_t>, 8> waiters;
  notifierHandles->ForEach([&](HAL_NotifierHandle handle, Notifier* notifier) {
    std::scoped_lock lock(notifier->mutex);
    // only wait for it if it's going to wake up (either because
    // the timeout has expired or the alarm hasn't been waited on yet)
    if (notifier->running &&
        (notifier->count == 0 || curTime >= notifier->waitTime)) {
      waiters.emplace_back(handle, notifier->count);
      notifier->cond.notify_all();
    }
  });
  for (;;) {
    int count = 0;
    int end = waiters.size();
    while (count < end) {
      auto& it = waiters[count];
      if (auto notifier = notifierHandles->Get(it.first)) {
        std::scoped_lock lock(notifier->mutex);
        if (notifier->active && notifier->count == it.second) {
          ++count;
          continue;
        }
      }
      // no longer need to wait for it, put at end so it can be erased
      it.swap(waiters[--end]);
    }
    if (count == 0) break;
    waiters.resize(count);
    notifiersWaiterCond.wait_for(ulock, std::chrono::duration<double>(1));
  }
}
}  // namespace hal

extern "C" {

HAL_NotifierHandle HAL_InitializeNotifier(int32_t* status) {
  hal::init::CheckInit();
  std::shared_ptr<Notifier> notifier = std::make_shared<Notifier>();
  HAL_NotifierHandle handle = notifierHandles->Allocate(notifier);
  if (handle == HAL_kInvalidHandle) {
    *status = HAL_HANDLE_ERROR;
    return HAL_kInvalidHandle;
  }
  return handle;
}

void HAL_SetNotifierName(HAL_NotifierHandle notifierHandle, const char* name,
                         int32_t* status) {
  auto notifier = notifierHandles->Get(notifierHandle);
  if (!notifier) return;
  std::scoped_lock lock(notifier->mutex);
  notifier->name = name;
}

void HAL_StopNotifier(HAL_NotifierHandle notifierHandle, int32_t* status) {
  auto notifier = notifierHandles->Get(notifierHandle);
  if (!notifier) return;

  {
    std::scoped_lock lock(notifier->mutex);
    notifier->active = false;
    notifier->running = false;
  }
  notifier->cond.notify_all();
}

void HAL_CleanNotifier(HAL_NotifierHandle notifierHandle, int32_t* status) {
  auto notifier = notifierHandles->Free(notifierHandle);
  if (!notifier) return;

  // Just in case HAL_StopNotifier() wasn't called...
  {
    std::scoped_lock lock(notifier->mutex);
    notifier->active = false;
    notifier->running = false;
  }
  notifier->cond.notify_all();
}

void HAL_UpdateNotifierAlarm(HAL_NotifierHandle notifierHandle,
                             uint64_t triggerTime, int32_t* status) {
  auto notifier = notifierHandles->Get(notifierHandle);
  if (!notifier) return;

  {
    std::scoped_lock lock(notifier->mutex);
    notifier->waitTime = triggerTime;
    notifier->running = (triggerTime != UINT64_MAX);
  }

  // We wake up any waiters to change how long they're sleeping for
  notifier->cond.notify_all();
}

void HAL_CancelNotifierAlarm(HAL_NotifierHandle notifierHandle,
                             int32_t* status) {
  auto notifier = notifierHandles->Get(notifierHandle);
  if (!notifier) return;

  {
    std::scoped_lock lock(notifier->mutex);
    notifier->running = false;
  }
}

uint64_t HAL_WaitForNotifierAlarm(HAL_NotifierHandle notifierHandle,
                                  int32_t* status) {
  auto notifier = notifierHandles->Get(notifierHandle);
  if (!notifier) return 0;

  std::unique_lock ulock(notifiersWaiterMutex);
  std::unique_lock lock(notifier->mutex);
  ++notifier->count;
  ulock.unlock();
  notifiersWaiterCond.notify_all();
  while (notifier->active) {
    uint64_t curTime = HAL_GetFPGATime(status);
    if (notifier->running && curTime >= notifier->waitTime) {
      notifier->running = false;
      return curTime;
    }

    double waitTime;
    if (!notifier->running || notifiersPaused) {
      // If not running, wait 1000 seconds
      waitTime = 1000.0;
    } else {
      waitTime = (notifier->waitTime - curTime) * 1e-6;
    }

    notifier->cond.wait_for(lock, std::chrono::duration<double>(waitTime));
  }
  return 0;
}

uint64_t HALSIM_GetNextNotifierTimeout(void) {
  uint64_t timeout = UINT64_MAX;
  notifierHandles->ForEach([&](HAL_NotifierHandle, Notifier* notifier) {
    std::scoped_lock lock(notifier->mutex);
    if (notifier->active && notifier->running && timeout > notifier->waitTime)
      timeout = notifier->waitTime;
  });
  return timeout;
}

int32_t HALSIM_GetNumNotifiers(void) {
  int32_t count = 0;
  notifierHandles->ForEach([&](HAL_NotifierHandle, Notifier* notifier) {
    std::scoped_lock lock(notifier->mutex);
    if (notifier->active) ++count;
  });
  return count;
}

int32_t HALSIM_GetNotifierInfo(struct HALSIM_NotifierInfo* arr, int32_t size) {
  int32_t num = 0;
  notifierHandles->ForEach([&](HAL_NotifierHandle handle, Notifier* notifier) {
    std::scoped_lock lock(notifier->mutex);
    if (!notifier->active) return;
    if (num < size) {
      arr[num].handle = handle;
      if (notifier->name.empty()) {
        std::snprintf(arr[num].name, sizeof(arr[num].name), "Notifier%d",
                      static_cast<int>(getHandleIndex(handle)));
      } else {
        std::strncpy(arr[num].name, notifier->name.c_str(),
                     sizeof(arr[num].name));
        arr[num].name[sizeof(arr[num].name) - 1] = '\0';
      }
      arr[num].timeout = notifier->waitTime;
      arr[num].running = notifier->running;
    }
    ++num;
  });
  return num;
}

}  // extern "C"
