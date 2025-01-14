/*************************************************************************
NAFO Forum Moderation Firehose Client
Copyright (c) Steve Townsend 2024

>>> SOURCE LICENSE >>>
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation (www.fsf.org); either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

A copy of the GNU General Public License is available at
http://www.fsf.org/licensing/licenses
>>> END OF LICENSE >>>
*************************************************************************/

#include "activity/event_recorder.hpp"
#include "metrics.hpp"

namespace activity {
event_recorder::event_recorder() : _queue(MaxBacklog) {
  _thread = std::thread([&] {
    static size_t matches(0);
    while (true) {
      timed_event my_payload;
      _queue.wait_dequeue(my_payload);
      metrics::instance()
          .operational_stats()
          .Get({{"events", "backlog"}})
          .Decrement();

      // record the activity
      _events.record(my_payload);

      // TODO terminate gracefully
    }
  });
}

void event_recorder::wait_enqueue(timed_event &&value) {
  _queue.wait_enqueue(value);
  metrics::instance()
      .operational_stats()
      .Get({{"events", "backlog"}})
      .Increment();
}

} // namespace activity
