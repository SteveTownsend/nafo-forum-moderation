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

#include "metrics.hpp"
#include "firehost_client_config.hpp"
#include "helpers.hpp"

metrics::metrics()
    : _registry(new prometheus::Registry),
      _matched_elements(
          add_counter("message_field_matches",
                      "Number of matches within each field of message")),
      _firehose_stats(
          add_counter("firehose", "Statistics about received firehose data")),
      _firehose_facets(add_histogram(
          "firehose_facets", "Statistics about received firehose facets")),
      _operational_stats(
          add_gauge("operational_stats", "Statistics about client internals")),
      _realtime_alerts(
          add_counter("realtime_alerts",
                      "Alerts generated for possibly suspect activity")) {
  // Histogram metrics have to be added by hand, on-deman instantiation is not
  // possible
  prometheus::Histogram::BucketBoundaries boundaries = {
      0.0,  1.0,  2.0,  3.0,  4.0,  5.0,  6.0,  7.0,  8.0,  9.0,  10.0,
      11.0, 12.0, 13.0, 14.0, 15.0, 16.0, 17.0, 18.0, 19.0, 20.0, 21.0,
      22.0, 23.0, 24.0, 25.0, 26.0, 27.0, 28.0, 29.0, 30.0};
  _firehose_facets.Add({{"facet", std::string(bsky::AppBskyRichtextFacetLink)}},
                       boundaries);
  _firehose_facets.Add(
      {{"facet", std::string(bsky::AppBskyRichtextFacetMention)}}, boundaries);
  _firehose_facets.Add({{"facet", std::string(bsky::AppBskyRichtextFacetTag)}},
                       boundaries);
  _firehose_facets.Add({{"facet", "total"}}, boundaries);
}

metrics &metrics::instance() {
  static metrics my_instance;
  return my_instance;
}

void metrics::set_config(std::shared_ptr<config> &settings) {
  _settings = settings;
  _port = _settings->get_config()[PROJECT_NAME]["metrics"]["port"]
              .as<std::string>();
  _exposer = std::make_unique<prometheus::Exposer>("0.0.0.0:" + _port);
  // ask the exposer to scrape the registry on incoming HTTP requests
  _exposer->RegisterCollectable(_registry);
}

prometheus::Family<prometheus::Counter> &
metrics::add_counter(std::string const &name, std::string const &help) {
  return prometheus::BuildCounter().Name(name).Help(help).Register(*_registry);
}

prometheus::Family<prometheus::Gauge> &
metrics::add_gauge(std::string const &name, std::string const &help) {
  return prometheus::BuildGauge().Name(name).Help(help).Register(*_registry);
}

prometheus::Family<prometheus::Histogram> &
metrics::add_histogram(std::string const &name, std::string const &help) {
  return prometheus::BuildHistogram().Name(name).Help(help).Register(
      *_registry);
}
