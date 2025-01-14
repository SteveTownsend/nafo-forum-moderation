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

#include "post_processor.hpp"
#include "activity/account_events.hpp"
#include "parser.hpp"

jetstream_payload::jetstream_payload() {}
jetstream_payload::jetstream_payload(std::string json_msg,
                                     match_results matches)
    : _json_msg(json_msg), _matches(matches) {}

void jetstream_payload::handle(post_processor<jetstream_payload> &) {
  // TODO almost identical to jetstream_payload::handle
  // Publish metrics for matches
  for (auto &result : _matches) {
    // this is the substring of the full JSON that matched one or more
    // desired strings
    REL_INFO("Candidate {}|{}|{}\nmatches {}\non message:{}",
             result._candidate._type, result._candidate._field,
             result._candidate._value, result._matches, _json_msg);
    for (auto const &match : result._matches) {
      prometheus::Labels labels(
          {{"type", result._candidate._type},
           {"field", result._candidate._field},
           {"filter", wstring_to_utf8(match.get_keyword())}});
      metrics::instance().matched_elements().Get(labels).Increment();
    }
  }
}

firehose_payload::firehose_payload() {}
firehose_payload::firehose_payload(parser &my_parser)
    : _parser(std::move(my_parser)) {}
void firehose_payload::handle(post_processor<firehose_payload> &processor) {
  auto const &other_cbors(_parser.other_cbors());
  if (other_cbors.size() != 2) {
    std::ostringstream oss;
    for (auto const &cbor : _parser.other_cbors()) {
      oss << cbor.second.dump();
    }
    REL_ERROR("Malformed firehose message {}", oss.str());
    return;
  }
  auto const &header(other_cbors.front().second);
  auto const &message(other_cbors.back().second);
  REL_DEBUG("Firehose header:  {}", dump_json(header));
  REL_DEBUG("         message: {}", dump_json(message));
  int op(header["op"].template get<int>());
  if (op == static_cast<int>(firehose::op::error)) {
    metrics::instance().firehose_stats().Get({{"op", "error"}}).Increment();
  } else if (op == static_cast<int>(firehose::op::message)) {
    metrics::instance().firehose_stats().Get({{"op", "message"}}).Increment();
    std::string op_type(header["t"].template get<std::string>());
    metrics::instance()
        .firehose_stats()
        .Get({{"op", "message"}, {"type", op_type}})
        .Increment();
    std::string repo;
    parser block_parser;
    if (op_type == firehose::OpTypeCommit) {
      repo = message["repo"].template get<std::string>();
      if (message.contains("blocks")) {
        // CAR file - nested in-situ parse to extract as JSON
        auto blocks(message["blocks"].template get<nlohmann::json::binary_t>());
        bool parsed(block_parser.json_from_car(blocks.cbegin(), blocks.cend()));
        if (parsed) {
          DBG_DEBUG("Commit content blocks: {}",
                    block_parser.dump_parse_content());
          DBG_DEBUG("Commit other blocks: {}", block_parser.dump_parse_other());
          auto const &matchable_cbors(block_parser.matchable_cbors());
          for (auto const &cbor : matchable_cbors) {
            auto candidates(
                block_parser.get_candidates_from_record(cbor.second));
            if (!candidates.empty()) {
              _candidates.insert(_candidates.end(), candidates.cbegin(),
                                 candidates.cend());
            }
          }
        } else {
          // TODO error handling
        }
      }
      for (auto const &oper : message["ops"]) {
        size_t count = 0;
        auto path(oper["path"].template get<std::string>());
        auto kind(oper["action"].template get<std::string>());
        for (const auto token : std::views::split(path, '/')) {
          // with string_view's C++23 range constructor:
          std::string field(token.cbegin(), token.cend());
          switch (count) {
          case 0:
            if (field.empty())
              throw std::invalid_argument("Blank collection in op.path " +
                                          path);
            metrics::instance()
                .firehose_stats()
                .Get({{"op", "message"},
                      {"type", op_type},
                      {"collection", field},
                      {"kind", kind}})
                .Increment();
            break;
          case 1:
            if (field.empty())
              throw std::invalid_argument("Blank key in op.path " + path);
            break;
          }
          ++count;
        }
        if (oper.contains("cid") && !oper["cid"].is_null()) {
          auto cid(oper["cid"].template get<nlohmann::json::binary_t>());
          parser cid_parser;
          if (cid_parser.json_from_cid(cid.cbegin(), cid.cend()) &&
              !_path_by_cid.insert({cid_parser.block_cid(), path}).second) {
            // We see this for Block operations very rarely. Log to try to track
            // it down
            REL_ERROR(
                "Duplicate cid {} at op.path {}, already used for path {}",
                cid_parser.block_cid(), path,
                _path_by_cid.find(cid_parser.block_cid())->second);
            REL_ERROR("Firehose header:  {}", dump_json(header));
            REL_ERROR("         message: {}", dump_json(message));
            REL_ERROR("Content CBORs:  {}", block_parser.dump_parse_content());
            REL_ERROR("Matched CBORs:  {}", block_parser.dump_parse_matched());
            REL_ERROR("Other CBORs:    {}", block_parser.dump_parse_other());
          }
        }
      }
      // handle all the CBORs with content, metrics, checking
      for (auto const &content_cbor : block_parser.content_cbors()) {
        handle_content(processor, repo, content_cbor.first,
                       content_cbor.second);
      }
      for (auto const &matchable_cbor : block_parser.matchable_cbors()) {
        handle_content(processor, repo, matchable_cbor.first,
                       matchable_cbor.second);
      }
    } else if (op_type == firehose::OpTypeIdentity ||
               op_type == firehose::OpTypeHandle) {
      repo = message["did"].template get<std::string>();
      if (message.contains("handle")) {
        std::string handle(message["handle"].template get<std::string>());
        _candidates.emplace_back(op_type, "handle", handle);
        processor.request_recording(
            {repo,
             bsky::time_stamp_from_iso_8601(
                 message["time"].template get<std::string>()),
             activity::handle(handle)});
      }
    } else if (op_type == firehose::OpTypeAccount) {
      repo = message["did"].template get<std::string>();
      bool active(message["active"].template get<bool>());
      metrics::instance()
          .firehose_stats()
          .Get({{"op", "message"},
                {"type", op_type},
                {"status", active ? "active" : "inactive"}})
          .Increment();
      if (active) {
        processor.request_recording(
            {repo,
             bsky::time_stamp_from_iso_8601(
                 message["time"].template get<std::string>()),
             activity::active()});
      } else if (message.contains("status")) {
        processor.request_recording(
            {repo,
             bsky::time_stamp_from_iso_8601(
                 message["time"].template get<std::string>()),
             activity::inactive(bsky::down_reason_from_string(
                 message["status"].template get<std::string>()))});
      } else {
        processor.request_recording(
            {repo,
             bsky::time_stamp_from_iso_8601(
                 message["time"].template get<std::string>()),
             activity::inactive(bsky::down_reason::unknown)});
      }
    } else if (op_type == firehose::OpTypeTombstone) {
      repo = message["did"].template get<std::string>();
      processor.request_recording(
          {repo,
           bsky::time_stamp_from_iso_8601(
               message["time"].template get<std::string>()),
           activity::inactive(bsky::down_reason::tombstone)});
    } else if (op_type == firehose::OpTypeMigrate ||
               op_type == firehose::OpTypeInfo) {
      // no-op
    }
    REL_TRACE("{} {}", header.dump(), message.dump());
    if (!_candidates.empty()) {
      auto matches(
          processor.get_matcher().all_matches_for_candidates(_candidates));
      if (!matches.empty()) {
        // Publish metrics for matches
        size_t count(0);
        for (auto const &result : matches) {
          // this is the substring of the full JSON that matched one or more
          // desired strings
          REL_INFO("{} matched candidate {}|{}|{}|{}", result._matches, repo,
                   result._candidate._type, result._candidate._field,
                   result._candidate._value);
          count += result._matches.size();
          for (auto const &match : result._matches) {
            prometheus::Labels labels(
                {{"type", result._candidate._type},
                 {"field", result._candidate._field},
                 {"filter", wstring_to_utf8(match.get_keyword())}});
            metrics::instance().matched_elements().Get(labels).Increment();
          }
        }
        // only log message once - might be interleaved with other thread output
        if (op_type == firehose::OpTypeCommit) {
          // curate a smaller version of the full message for correlation
          REL_INFO("in message: {} {} {}", repo, dump_json(message["ops"]),
                   block_parser.dump_parse_content());
        } else {
          REL_INFO("in message: {} {}", repo, dump_json(message));
        }
        // record suspect activity as a special-case event
        processor.request_recording(
            {repo, bsky::current_time(), activity::matches(count)});
      }
    }
  }
}

void firehose_payload::handle_content(
    post_processor<firehose_payload> &processor, std::string const &repo,
    std::string const &cid, nlohmann::json const &content) {
  std::string this_path;
  if (_path_by_cid.contains(cid)) {
    this_path = _path_by_cid[cid];
  } else {
    throw std::runtime_error("cannot get URI for cid at " + dump_json(content));
  }
  auto collection(content["$type"].template get<std::string>());
  bsky::tracked_event event_type = bsky::event_type_from_collection(collection);
  if (event_type == bsky::tracked_event::post) {
    bool recorded(false);
    // Post create/update - may need qualification
    if (content.contains("reply")) {
      event_type = bsky::tracked_event::reply;
      recorded = true;
      processor.request_recording(
          {repo,
           bsky::time_stamp_from_iso_8601(
               content["createdAt"].template get<std::string>()),
           activity::reply(
               this_path,
               content["reply"]["root"]["uri"].template get<std::string>(),
               content["reply"]["parent"]["uri"].template get<std::string>())});
    }
    // Check facets
    // 1. look for Matryoshka post - embed video/images, multiple facet
    // mentions/tags
    // https://github.com/SteveTownsend/nafo-forum-moderation/issues/68
    // 2. check URIs for toxic content
    size_t tags(0);
    if (content.contains("tags")) {
      tags = content["tags"].size();
    }
    if (content.contains("embed")) {
      auto const &embed(content["embed"]);
      std::string embed_type_str(embed["$type"].template get<std::string>());
      bsky::embed_type embed_type =
          bsky::embed_type_from_string(embed_type_str);
      if (embed_type == bsky::embed_type::record ||
          embed_type == bsky::embed_type::record_with_media) {
        // Embedded record, this is a quote post
        event_type = bsky::tracked_event::quote;
        recorded = true;
        processor.request_recording(
            {repo,
             bsky::time_stamp_from_iso_8601(
                 content["createdAt"].template get<std::string>()),
             activity::quote(
                 this_path,
                 embed_type == bsky::embed_type::record
                     ? embed["record"]["uri"].template get<std::string>()
                     : embed["record"]["record"]["uri"]
                           .template get<std::string>())});
      }
      if (content.contains("facets")) {
        size_t mentions(0);
        size_t links(0);
        if (embed_type == bsky::embed_type::video && embed.contains("langs")) {
          // count languages in video
          auto langs(embed["langs"].template get<std::vector<std::string>>());
          for (auto const &lang : langs) {
            metrics::instance()
                .firehose_stats()
                .Get({{"embed", embed_type_str}, {"language", lang}})
                .Increment();
          }
        }
        // bool is_media(embed_type == bsky::AppBskyEmbedImages ||
        //               embed_type == bsky::AppBskyEmbedVideo);
        bool has_facets(false);
        for (auto const &facet : content["facets"]) {
          has_facets = true;
          for (auto const &feature : facet["features"]) {
            auto const &facet_type(
                feature["$type"].template get<std::string>());
            // if (is_media) {
            if (facet_type == bsky::AppBskyRichtextFacetMention) {
              ++mentions;
            } else if (facet_type == bsky::AppBskyRichtextFacetTag) {
              ++tags;
              // }
            } else if (facet_type == bsky::AppBskyRichtextFacetLink) {
              _candidates.emplace_back(
                  collection, std::string(bsky::AppBskyRichtextFacetLink),
                  feature["uri"].template get<std::string>());
              ++links;
            }
          }
        }
        // record metrics for facet types by embed type
        if (mentions > 0) {
          metrics::instance()
              .firehose_facets()
              .GetAt(
                  {{"facet", std::string(bsky::AppBskyRichtextFacetMention)}})
              .Observe(static_cast<double>(mentions));
          if (mentions > activity::account::MentionFacetThreshold) {
            processor.request_recording(
                {repo,
                 bsky::time_stamp_from_iso_8601(
                     content["createdAt"].template get<std::string>()),
                 activity::mentions(mentions)});
          }
        }
        if (links > 0) {
          metrics::instance()
              .firehose_facets()
              .GetAt({{"facet", std::string(bsky::AppBskyRichtextFacetLink)}})
              .Observe(static_cast<double>(links));
          if (links > activity::account::MentionFacetThreshold) {
            processor.request_recording(
                {repo,
                 bsky::time_stamp_from_iso_8601(
                     content["createdAt"].template get<std::string>()),
                 activity::links(links)});
          }
        }
        if (tags > 0) {
          metrics::instance()
              .firehose_facets()
              .GetAt({{"facet", std::string(bsky::AppBskyRichtextFacetTag)}})
              .Observe(static_cast<double>(tags));
          if (tags > activity::account::TagFacetThreshold) {
            processor.request_recording(
                {repo,
                 bsky::time_stamp_from_iso_8601(
                     content["createdAt"].template get<std::string>()),
                 activity::tags(tags)});
          }
        }
        if (has_facets) {
          size_t total(mentions + tags + links);
          metrics::instance()
              .firehose_facets()
              .GetAt({{"facet", "total"}})
              .Observe(static_cast<double>(total));
          if (total > activity::account::TotalFacetThreshold) {
            processor.request_recording(
                {repo,
                 bsky::time_stamp_from_iso_8601(
                     content["createdAt"].template get<std::string>()),
                 activity::facets(total)});
          }
        }
        if (content.contains("langs")) {
          auto langs(content["langs"].template get<std::vector<std::string>>());
          for (auto const &lang : langs) {
            metrics::instance()
                .firehose_stats()
                .Get({{"collection", collection}, {"language", lang}})
                .Increment();
          }
        }
      }
    }
    if (!recorded) {
      // plain old post, not a reply or quote
      processor.request_recording(
          {repo,
           bsky::time_stamp_from_iso_8601(
               content["createdAt"].template get<std::string>()),
           activity::post(this_path)});
    }
  } else if (event_type == bsky::tracked_event::block) {
    processor.request_recording(
        {repo,
         bsky::time_stamp_from_iso_8601(
             content["createdAt"].template get<std::string>()),
         activity::block(this_path,
                         content["subject"].template get<std::string>())});
  } else if (event_type == bsky::tracked_event::follow) {
    processor.request_recording(
        {repo,
         bsky::time_stamp_from_iso_8601(
             content["createdAt"].template get<std::string>()),
         activity::follow(this_path,
                          content["subject"].template get<std::string>())});
  } else if (event_type == bsky::tracked_event::like) {
    processor.request_recording(
        {repo,
         bsky::time_stamp_from_iso_8601(
             content["createdAt"].template get<std::string>()),
         activity::like(
             this_path,
             content["subject"]["uri"].template get<std::string>())});
  } else if (event_type == bsky::tracked_event::profile) {
    processor.request_recording(
        {repo,
         (content.contains("createdAt")
              ? bsky::time_stamp_from_iso_8601(
                    content["createdAt"].template get<std::string>())
              : bsky::current_time()),
         activity::profile(this_path)});
  } else if (event_type == bsky::tracked_event::repost) {
    processor.request_recording(
        {repo,
         bsky::time_stamp_from_iso_8601(
             content["createdAt"].template get<std::string>()),
         activity::repost(
             this_path,
             content["subject"]["uri"].template get<std::string>())});
  }
}
