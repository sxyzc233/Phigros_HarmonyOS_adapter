#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <cstdint>

namespace rea_prefs {
enum class Type : uint8_t { String=1, Int=2, Long=3, Bool=4, Float=5 };
struct Value { Type type=Type::String; std::string text; int64_t integer=0; float number=0; };
using Values=std::map<std::string,Value>;
struct Store { std::mutex mutex; std::string name,path; Values values; bool mainValid=true; };
struct Editor { std::mutex mutex; std::shared_ptr<Store> store; Values pending; std::set<std::string> removed; bool clear=false; };
std::shared_ptr<Store> Open(const std::string &name);
bool Read(const std::shared_ptr<Store> &store,const std::string &key,Value &out);
Values Snapshot(const std::shared_ptr<Store> &store);
bool Apply(const std::shared_ptr<Editor> &editor);
bool SelfTest();
}
