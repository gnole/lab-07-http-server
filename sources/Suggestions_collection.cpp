// Copyright 2020 Your Name <your_email>

#include "Suggestions_collection.hpp"

#include <algorithm>

Suggestions_collection::Suggestions_collection() { suggestions_ = {}; }
void Suggestions_collection::Update(json storage) {
  std::sort(storage.begin(), storage.end(),
            [](const json& a, const json& b) -> bool {
              return a.at("cost") < b.at("cost");
            });
  suggestions_ = storage;
}
json Suggestions_collection::Suggest(const std::string& input) {
  json result;
  for (size_t i = 0, m = 0; i < suggestions_.size(); ++i) {
    if (input == suggestions_[i].at("id")) {
      json pos;
      pos["text"] = suggestions_[i].at("name");
      pos["position"] = m++;
      result["suggestions"].push_back(pos);
    }
  }
  return result;
}
