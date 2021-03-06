// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/test/launcher/test_launcher_tracer.h"

#include "base/json/json_file_value_serializer.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"

namespace base {

TestLauncherTracer::TestLauncherTracer()
    : trace_start_time_(TimeTicks::Now()) {}

TestLauncherTracer::~TestLauncherTracer() = default;

int TestLauncherTracer::RecordProcessExecution(TimeTicks start_time,
                                               TimeDelta duration) {
  AutoLock lock(lock_);

  int process_num = events_.size();
  Event event;
  event.name = StringPrintf("process #%d", process_num);
  event.timestamp = start_time;
  event.duration = duration;
  event.thread_id = PlatformThread::CurrentId();
  events_.push_back(event);
  return process_num;
}

bool TestLauncherTracer::Dump(const FilePath& path) {
  AutoLock lock(lock_);

  Value::ListStorage json_events_storage;
  for (const Event& event : events_) {
    Value json_event(Value::Type::DICTIONARY);
    json_event.SetStringKey("name", event.name);
    json_event.SetStringKey("ph", "X");
    json_event.SetIntKey(
        "ts", (event.timestamp - trace_start_time_).InMicroseconds());
    json_event.SetIntKey("dur", event.duration.InMicroseconds());
    json_event.SetIntKey("tid", event.thread_id);

    // Add fake values required by the trace viewer.
    json_event.SetIntKey("pid", 0);

    json_events_storage.push_back(std::move(json_event));
  }

  JSONFileValueSerializer serializer(path);
  return serializer.Serialize(Value(std::move(json_events_storage)));
}

}  // namespace base
