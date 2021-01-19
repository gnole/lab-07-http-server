// Copyright 2020 Your Name <your_email>

#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include "Json_storage.hpp"

Json_storage::Json_storage(const std::string& filename) : filename_(filename) {}
json Json_storage::get_storage() const { return storage_; }
void Json_storage::Load() {
  try {
    std::ifstream in(filename_);
    in >> storage_;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
  }
}
