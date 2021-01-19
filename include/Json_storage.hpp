// Copyright 2020 Your Name <your_email>

#ifndef INCLUDE_JSON_STORAGE_HPP_
#define INCLUDE_JSON_STORAGE_HPP_

#include <nlohmann/json.hpp>
#include <string>
using json = nlohmann::json;

class Json_storage {
 public:
  explicit Json_storage(const std::string& filename);
  json get_storage() const;
  void Load();

 private:
  std::string filename_;
  json storage_;
};

#endif  // INCLUDE_JSON_STORAGE_HPP_
