#ifndef __post_processor_hpp__
#define __post_processor_hpp__
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
#include "helpers.hpp"
#include "log_wrapper.hpp"
#include "matcher.hpp"
#include "metrics.hpp"
#include "queue/readerwritercircularbuffer.h"
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <thread>
#include <unordered_map>

namespace firehose {

constexpr size_t QueueLimit = 10000;

enum class op { error = -1, message = 1 };

enum class op_type {
  account = 1,
  commit,
  handle,
  identity,
  info,
  migrate,
  tombstone,
  invalid = -1
};

constexpr std::string_view OpTypeAccount = "#account";
constexpr std::string_view OpTypeCommit = "#commit";
constexpr std::string_view OpTypeHandle = "#handle";
constexpr std::string_view OpTypeIdentity = "#identity";
constexpr std::string_view OpTypeInfo = "#info";
constexpr std::string_view OpTypeMigrate = "#migrate";
constexpr std::string_view OpTypeTombstone = "#tombstone";

inline op_type op_type_from_string(std::string const &op_type_str) {
  if (op_type_str == OpTypeAccount) {
    return op_type::account;
  } else if (op_type_str == OpTypeCommit) {
    return op_type::commit;
  } else if (op_type_str == OpTypeHandle) {
    return op_type::handle;
  } else if (op_type_str == OpTypeIdentity) {
    return op_type::identity;
  } else if (op_type_str == OpTypeInfo) {
    return op_type::info;
  } else if (op_type_str == OpTypeMigrate) {
    return op_type::migrate;
  } else if (op_type_str == OpTypeTombstone) {
    return op_type::tombstone;
  }
  return op_type::invalid;
}

enum class op_kind { create = 1, delete_, update, invalid = -1 };

constexpr std::string_view OpKindCreate = "create";
constexpr std::string_view OpKindDelete = "delete";
constexpr std::string_view OpKindUpdate = "update";

inline op_kind op_kind_from_string(std::string const &op_kind_str) {
  if (op_kind_str == OpKindCreate) {
    return op_kind::create;
  } else if (op_kind_str == OpKindDelete) {
    return op_kind::delete_;
  } else if (op_kind_str == OpKindUpdate) {
    return op_kind::update;
  }
  return op_kind::invalid;
}

} // namespace firehose

template <typename T> class post_processor {
public:
  post_processor() : _queue(firehose::QueueLimit) {
    _thread = std::thread([&] {
      static size_t matches(0);
      while (true) {
        T my_payload;
        _queue.wait_dequeue(my_payload);
        metrics::instance()
            .operational_stats()
            .Get({{"message", "backlog"}})
            .Decrement();

        my_payload.handle(*this);
        // TODO auto-report if rule indicates, ignoring dups
        // TODO avoid spam for automated posts

        // TODO terminate gracefully
      }
    });
  }
  ~post_processor() = default;
  void wait_enqueue(T &&value) {
    _queue.wait_enqueue(value);
    metrics::instance()
        .operational_stats()
        .Get({{"message", "backlog"}})
        .Increment();
  }
  inline void set_matcher(std::shared_ptr<matcher> my_matcher) {
    _matcher = my_matcher;
  }
  inline matcher &get_matcher() { return *_matcher; }
  inline void request_recording(activity::timed_event &&event) {
    _recorder.wait_enqueue(std::move(event));
  }

private:
  // Declare queue between websocket and match post-processing
  moodycamel::BlockingReaderWriterCircularBuffer<T> _queue;
  std::thread _thread;
  std::shared_ptr<matcher> _matcher;
  activity::event_recorder _recorder;
};

class jetstream_payload {
public:
  jetstream_payload();
  jetstream_payload(std::string json_msg, match_results matches);
  void handle(post_processor<jetstream_payload> &processor);

private:
  std::string _json_msg;
  match_results _matches;
};

class firehose_payload {
public:
  firehose_payload();
  firehose_payload(parser &my_parser);
  void handle(post_processor<firehose_payload> &processor);

private:
  void handle_content(post_processor<firehose_payload> &processor,
                      std::string const &repo, std::string const &cid,
                      nlohmann::json const &content);
  parser _parser;
  candidate_list _candidates;
  std::unordered_map<std::string, std::string> _path_by_cid;
};

#endif