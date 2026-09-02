#include "abi_mangle_encoder.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace abi_mangle {
namespace {

std::string normalized_name(const std::string & name)
{
  return name.compare(0, 2, "::") == 0 ? name.substr(2) : name;
}

std::vector<std::string> name_components(const std::string & name)
{
  const std::string normalized = normalized_name(name);
  std::vector<std::string> components;
  std::size_t start = 0;
  while(start <= normalized.size()) {
    const std::size_t separator = normalized.find("::", start);
    if(separator == std::string::npos) {
      if(start == normalized.size()) {
        throw std::logic_error("ABI qualified name has an empty component");
      }
      components.push_back(normalized.substr(start));
      break;
    }
    if(separator == start) {
      throw std::logic_error("ABI qualified name has an empty component");
    }
    components.push_back(normalized.substr(start, separator - start));
    start = separator + 2;
  }
  if(components.empty()) {
    throw std::logic_error("ABI qualified name is empty");
  }
  return components;
}

std::string joined_components(const std::vector<std::string> & components,
                              std::size_t last)
{
  std::string result;
  for(std::size_t i = 0; i <= last; ++i) {
    if(i != 0) result += "::";
    result += components[i];
  }
  return result;
}

std::string source_name(const std::string & name)
{
  return std::to_string(name.size()) + name;
}

bool needs_nested_name(const std::vector<std::string> & components)
{
  if(components.size() == 1) return false;
  return !(components.size() == 2 && components[0] == "std");
}

std::vector<std::string> split_fact_line(const std::string & line)
{
  std::istringstream input(line);
  std::vector<std::string> words;
  std::string word;
  while(input >> word) {
    words.push_back(word);
  }
  return words;
}

void append_stream_case(const AbiFactCase & fact_case, std::string * output,
                        std::string * first_error)
{
  try {
    *output += mangle_fact_case(fact_case);
    *output += "\n";
  } catch(const std::exception & error) {
    if(first_error->empty()) {
      *first_error = error.what();
    }
  }
}

}  // namespace

std::string Mangler::mangle_prefix_chain(
  const std::string & qualified_name, bool register_last,
  const std::vector<std::string> & abi_tags)
{
  const std::vector<std::string> components = name_components(qualified_name);
  const bool standard = components[0] == "std";
  const std::size_t first_prefix = standard ? 1 : 0;
  std::string result;
  if(standard) {
    result = "St";
  }
  for(std::size_t i = first_prefix; i + 1 < components.size(); ++i) {
    const std::string key = joined_components(components, i);
    std::string spelling;
    if(substitutions_.lookup(key, &spelling)) {
      result += spelling;
    } else {
      result += source_name(components[i]);
      substitutions_.add(key);
    }
  }
  result += source_name(components.back());
  if(!abi_tags.empty()) {
    result += mangle_tag_list(abi_tags);
  }
  if(register_last) {
    substitutions_.add(normalized_name(qualified_name));
  }
  return result;
}

std::string Mangler::mangle_qualified_name(
  const std::string & qualified_name, bool register_last,
  const std::vector<std::string> & abi_tags)
{
  const std::vector<std::string> components = name_components(qualified_name);
  const bool nested = needs_nested_name(components);
  const std::string body = mangle_prefix_chain(qualified_name, register_last,
                                               abi_tags);
  return nested ? "N" + body + "E" : body;
}

std::string Mangler::mangle_internal_name(const std::string & qualified_name)
{
  const std::vector<std::string> components = name_components(qualified_name);
  if(components.size() == 1) {
    return "L" + source_name(components[0]);
  }
  std::string result = "N";
  for(std::size_t i = 0; i + 1 < components.size(); ++i) {
    result += source_name(components[i]);
  }
  result += "L" + source_name(components.back()) + "E";
  return result;
}

std::string Mangler::mangle_special_target(const AbiTargetRecord & target)
{
  if(target.kind == ABI_TARGET_FACT_TYPEINFO) {
    return "_ZTI" + mangle_type(target.type);
  }
  if(target.kind == ABI_TARGET_FACT_VTABLE) {
    return "_ZTV" + mangle_type(target.type);
  }
  if(target.kind == ABI_TARGET_FACT_VTT) {
    return "_ZTT" + mangle_type(target.type);
  }
  if(target.kind == ABI_TARGET_FACT_CONSTRUCTION_VTABLE) {
    return "_ZTC" + mangle_type(target.type) + std::to_string(target.base_offset) +
      "_" + mangle_type(target.base_type);
  }
  if(target.kind == ABI_TARGET_FACT_THREAD_LOCAL_WRAPPER) {
    return "_ZTW" + mangle_qualified_name(target.qualified_name, false);
  }
  if(target.kind == ABI_TARGET_FACT_VARIABLE) {
    return "_Z" + mangle_qualified_name(target.qualified_name, false);
  }
  if(target.kind == ABI_TARGET_FACT_TYPE) {
    return mangle_type(target.type);
  }
  throw std::logic_error("unsupported target");
}

std::string Mangler::mangle_target(const AbiTargetRecord & target)
{
  if(target.kind == ABI_TARGET_FACT_FUNCTION ||
     target.kind == ABI_TARGET_FACT_THUNK ||
     target.kind == ABI_TARGET_FACT_VIRTUAL_BASE_THUNK) {
    throw std::logic_error("unsupported target");
  }
  return mangle_special_target(target);
}

std::string mangle_fact_case(const AbiFactCase & fact_case)
{
  AbiDefinitionTable definitions;
  const AbiTargetRecord * target = 0;
  for(std::size_t i = 0; i < fact_case.records.size(); ++i) {
    const AbiFactRecord & record = fact_case.records[i];
    if(record.kind == ABI_FACT_RECORD_DEFINITION) {
      definitions.add(record.definition);
    } else if(record.kind == ABI_FACT_RECORD_TARGET) {
      if(target) {
        throw std::logic_error("ABI case must contain exactly one target");
      }
      target = &record.target;
    }
  }
  if(!target) {
    throw std::logic_error("ABI case must contain exactly one target");
  }
  return abi_mangle::Mangler(definitions).mangle_target(*target);
}

std::string mangle_fact_files(const std::vector<std::string> & input_paths)
{
  std::string output;
  std::string first_error;
  for(std::size_t i = 0; i < input_paths.size(); ++i) {
    std::ifstream input(input_paths[i].c_str());
    if(!input) {
      throw std::logic_error("unable to read ABI fact file '" + input_paths[i] +
        "'");
    }
    AbiFactCase current;
    bool active = false;
    std::string line;
    while(std::getline(input, line)) {
      const std::vector<std::string> words = split_fact_line(line);
      if(words.empty()) {
        continue;
      }
      if(words[0] == "case") {
        if(words.size() != 2) {
          throw std::logic_error("malformed case header");
        }
        if(active) {
          append_stream_case(current, &output, &first_error);
        }
        current = AbiFactCase();
        current.label = words[1];
        active = true;
        continue;
      }
      if(!active) {
        active = true;
        current = AbiFactCase();
      }
      current.records.push_back(parse_fact_record_words(words));
    }
    if(active) {
      append_stream_case(current, &output, &first_error);
    } else {
      throw std::logic_error("ABI fact file contains no cases");
    }
  }
  if(!first_error.empty()) {
    throw std::logic_error(first_error);
  }
  return output;
}

}  // namespace abi_mangle
