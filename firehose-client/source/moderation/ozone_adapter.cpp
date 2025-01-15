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
#include "moderation/ozone_adapter.hpp"
#include "log_wrapper.hpp"

namespace bsky {
namespace moderation {

ozone_adapter::ozone_adapter(std::string const &connection_string)
    : _connection_string(connection_string), _cx(connection_string) {
  REL_INFO("Connected OK to moderation DB: {}", safe_connection_string());
}

void ozone_adapter::start() {
  // TODO handle errors, reconnect to DB
  _thread = std::thread([&] {
    while (true) {
      // load the list of labeled accounts
      check_refresh_labeled();

      std::this_thread::sleep_for(ThreadDelay);
    }
  });
}

// Don't refresh until interval has elapsed
void ozone_adapter::check_refresh_labeled() {
  std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
  if (std::chrono::duration_cast<std::chrono::seconds>(
          now - _last_labeled_refresh) > LabeledAccountRefreshInterval) {
    decltype(_labeled_accounts) new_list;
    pqxx::work tx(_cx);
    for (auto [did] : tx.query<std::string>(
             "SELECT DISTINCT(\"subjectDid\") FROM moderation_event WHERE "
             "(action = 'tools.ozone.moderation.defs#modEventLabel')")) {
      new_list.insert(did);
    }
    std::lock_guard guard(_lock);
    _labeled_accounts.swap(new_list);
    _last_labeled_refresh = std::chrono::steady_clock::now();
  }
}

bool ozone_adapter::is_labeled(std::string const &did) const {
  std::lock_guard guard(_lock);
  return _labeled_accounts.contains(did);
}

// mask the password
std::string ozone_adapter::safe_connection_string() const {
  constexpr const char password_sentinel[] = "password=";
  constexpr const char *password_mask = "********";
  size_t start =
      _connection_string.find(password_sentinel, sizeof(password_sentinel) - 1);
  if (start != std::string::npos) {
    // find first subsequent space (or end of string) and replace the password
    start += sizeof(password_sentinel) - 1;
    size_t end = _connection_string.find(' ', start);
    if (end == std::string::npos) {
      end = _connection_string.length();
    }
    return std::string(_connection_string)
        .replace(start, end - start, password_mask);
  }
  return _connection_string;
}

} // namespace moderation
} // namespace bsky