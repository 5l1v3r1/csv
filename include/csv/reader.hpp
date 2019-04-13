/*
Simplified BSD license:
Copyright (c) 2019, Pranav Srinivas Kumar
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this 
list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice, this 
list of conditions and the following disclaimer in the documentation and/or 
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND 
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE 
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL 
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER 
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, 
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF 
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once
#include <csv/queue.hpp>
#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <string>
#include <sstream>
#include <map>
#include <thread>
#include <future>
#include <chrono>
#include <mutex>
#include <condition_variable>

namespace csv {

class reader {
public:
  reader() :
    filename_(""),
    delimiter_(","),
#ifdef _WIN32
    line_terminator_("\n"),
#else
    line_terminator_("\r\n"),
#endif
    quotechar_('"'),
    trim_characters_({}),
    columns_(0),
    ready_(false) {}

  ~reader() {
    thread_.join();
  }

  bool parse(const std::string& filename) {
    filename_ = filename;
    done_future_ = done_promise_.get_future();
    thread_ = std::thread(&reader::process_values, this, &done_future_);
    parse_internal();
    done();
    std::unique_lock<std::mutex> lock(ready_mutex_);
    while (!ready_) ready_cv_.wait(lock);
    return true;
  }

  reader& configure() {
    return *this;
  }

  reader& delimiter(const std::string& delimiter) {
    delimiter_ = delimiter;
    return *this;
  }

  reader& line_terminator(const std::string& line_terminator) {
    line_terminator_ = line_terminator;
    return *this;
  }

  reader& quotechar(char quotechar) {
    quotechar_ = quotechar;
    return *this;
  }

  // Base case for trim_characters parameter packing
  void trim_characters() {}

  // Parameter packed trim_characters method
  // Accepts a variadic number of characters
  template<typename T, typename... Targs>
  void trim_characters(T character, Targs... Fargs) {
    trim_characters_.push_back(character);
    trim_characters(Fargs...);
  }

  std::vector<std::map<std::string, std::string>> rows() {
    return rows_;
  }

  template <typename T>
  T get(size_t row, const std::string& key) {
    std::stringstream stream(rows_[row][key]);
    T result = T();
    stream >> result;
    return result;
  }

private:
  bool front(std::string& value) {
    return values_.try_dequeue(value);
  }

  void done() {
    done_promise_.set_value(true);
  }

  void parse_internal() {
    std::fstream stream(filename_, std::fstream::in);
    char ch;
    std::string current;
    bool first_row = true;
    size_t quotes_encountered = 0;
    while (stream >> std::noskipws >> ch) {

      // Handle delimiter
      for (size_t i = 0; i < delimiter_.size(); i++) {
        if (ch == delimiter_[i]) {
          if (i + 1 == delimiter_.size()) {
            // Make sure that an even number of quotes have been 
            // encountered so far
            // If not, then don't considered the delimiter
            if (quotes_encountered % 2 == 0) {
              if (first_row) columns_ += 1;
              values_.enqueue(trim(current));
              current = "";
              stream >> std::noskipws >> ch;
              quotes_encountered = 0;
            }
            else {
              current += ch;
              stream >> std::noskipws >> ch;
            }
          }
          else {
            if (quotes_encountered % 2 != 0) {
              current += ch;
            }
            stream >> std::noskipws >> ch;
          }
        } 
        else { 
          break;
        }
      }

      // Handle line_terminator
      for (size_t i = 0; i < line_terminator_.size(); i++) {
        if (ch == line_terminator_[i]) {
          if (i + 1 == line_terminator_.size()) {
            if (first_row) columns_ += 1;
            values_.enqueue(trim(current));
            current = "";
            stream >> std::noskipws >> ch;
            if (first_row) first_row = false;
          }
          else {
            stream >> std::noskipws >> ch;
          }
        } 
        else { 
          break;
        }
      }

      // Base case
      current += ch;
      if (ch == quotechar_)
        quotes_encountered += 1;
    }
    if (current != "")
      values_.enqueue(trim(current));
  }

  void process_values(std::future<bool> * future_object) {
    size_t header_index = 0;
    std::map<std::string, std::string> row;
    while(true) {
      std::string value;
      if (front(value)) {
        if (header_index < columns_) {
          headers_.push_back(value);
          header_index += 1;
        }
        else {
          row[headers_[header_index % headers_.size()]] = value;
          header_index += 1;
          if (header_index % headers_.size() == 0) {
            rows_.push_back(row);
            row.clear();
          }
        }
      }
      const auto future_status = 
        future_object->wait_for(std::chrono::seconds(0));
      if (future_status == std::future_status::ready && 
          values_.size_approx() == 0) {
        std::unique_lock<std::mutex> lock(ready_mutex_);
        ready_ = true;
        ready_cv_.notify_all();
        break;
      }
    }
  }

  // trim white spaces from the left end of an input string
  std::string ltrim(std::string input) {
    std::string result = input;
    result.erase(result.begin(), std::find_if(result.begin(), result.end(), [=](int ch) {
      return !(std::find(trim_characters_.begin(), trim_characters_.end(), ch) 
        != trim_characters_.end());
    }));
    return result;
  }

  // trim white spaces from right end of an input string
  std::string rtrim(std::string input) {
    std::string result = input;
    result.erase(std::find_if(result.rbegin(), result.rend(), [=](int ch) {
      return !(std::find(trim_characters_.begin(), trim_characters_.end(), ch) 
        != trim_characters_.end());
    }).base(), result.end());
    return result;
  }

  // trim white spaces from either end of an input string
  std::string trim(std::string input) {
    return ltrim(rtrim(input));
  }

  std::string filename_;
  std::string delimiter_;
  std::string line_terminator_;
  char quotechar_;
  std::vector<char> trim_characters_;
  size_t columns_;
  std::vector<std::string> headers_;
  std::vector<std::map<std::string, std::string>> rows_;
  bool ready_;
  std::condition_variable ready_cv_;
  std::mutex ready_mutex_;
  std::thread thread_;
  std::promise<bool> done_promise_;
  std::future<bool> done_future_;
  moodycamel::ConcurrentQueue<std::string> values_;
};

}