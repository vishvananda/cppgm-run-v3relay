#include "lowir_model.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <string>

namespace lowir_model {

namespace {

bool parse_positive_number(const std::string & text, std::size_t & value)
{
  if(text.empty() || text[0] == '-') return false;
  char * end = 0;
  errno = 0;
  const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
  if(errno != 0 || end == text.c_str() || *end != '\0' || parsed == 0 ||
     parsed > static_cast<unsigned long long>(static_cast<std::size_t>(-1))) {
    return false;
  }
  value = static_cast<std::size_t>(parsed);
  return true;
}

struct LowirToken
{
  std::string text;
  std::size_t line = 1;
  std::size_t column = 1;
};

bool has_prefix(const std::string & text, char prefix)
{
  return text.size() > 1 && text[0] == prefix;
}

bool is_delimiter(char ch)
{
  return std::isspace(static_cast<unsigned char>(ch)) != 0 ||
         ch == '(' || ch == ')' || ch == '{' || ch == '}' ||
         ch == '[' || ch == ']' || ch == ':' || ch == ',' ||
         ch == '=' || ch == '<' || ch == '>' || ch == '+' || ch == '!' ||
         ch == '#';
}

bool looks_like_float(const std::string & text)
{
  return text.find('.') != std::string::npos ||
         text.find('e') != std::string::npos ||
         text.find('E') != std::string::npos ||
         (!text.empty() && (text[text.size() - 1] == 'f' ||
                            text[text.size() - 1] == 'F' ||
                            text[text.size() - 1] == 'l' ||
                            text[text.size() - 1] == 'L'));
}

bool is_boundary_key(const std::string & key)
{
  return key == "arity" || key == "effects" || key == "unwind" ||
         key == "return";
}

}  // namespace

LowTypeInfo describe_low_type(const LowType & type)
{
  LowTypeInfo info;
  const std::string & text = type.text;
  if(text == "void") {
    info.kind = LowTypeInfo::LTI_VOID;
    info.alignment = 1;
    return info;
  }
  if(text == "ptr") {
    info.kind = LowTypeInfo::LTI_POINTER;
    info.bits = 64;
    info.bytes = 8;
    info.alignment = 8;
    return info;
  }
  const char * const integer_names[] = {
    "i1", "i8", "u8", "i16", "u16", "i32", "u32", "i64"
  };
  const unsigned integer_bits[] = { 1, 8, 8, 16, 16, 32, 32, 64 };
  for(std::size_t i = 0; i < sizeof(integer_names) / sizeof(integer_names[0]); ++i) {
    if(text == integer_names[i]) {
      info.kind = LowTypeInfo::LTI_INTEGER;
      info.bits = integer_bits[i];
      info.bytes = (info.bits + 7u) / 8u;
      info.alignment = info.bytes;
      info.signed_integer = text[0] == 'i';
      return info;
    }
  }
  if(text == "f32" || text == "f64" || text == "f80") {
    info.kind = LowTypeInfo::LTI_FLOAT;
    info.bits = text == "f32" ? 32u : (text == "f64" ? 64u : 80u);
    info.bytes = text == "f80" ? 16u : info.bits / 8u;
    info.alignment = info.bytes;
    return info;
  }
  if(text.size() > 6 && text.compare(0, 4, "obj<") == 0 &&
     text[text.size() - 1] == '>') {
    const std::string body = text.substr(4, text.size() - 5);
    const std::size_t x = body.find('x');
    std::size_t bytes = 0;
    std::size_t alignment = 0;
    if(x != std::string::npos && body.find('x', x + 1) == std::string::npos &&
       parse_positive_number(body.substr(0, x), bytes) &&
       parse_positive_number(body.substr(x + 1), alignment) &&
       (alignment & (alignment - 1)) == 0) {
      info.kind = LowTypeInfo::LTI_OBJECT;
      info.bytes = bytes;
      info.alignment = alignment;
      return info;
    }
  }
  throw ParseError("unknown or malformed LowIR type '" + text + "'");
}

class LowirLexer
{
public:
  LowirLexer(const std::string & text, const std::string & source_name)
    : text_(text), source_name_(source_name)
  {}

  LowirToken peek()
  {
    if(!has_peek_) {
      peek_token_ = read_token();
      has_peek_ = true;
    }
    return peek_token_;
  }

  LowirToken take()
  {
    LowirToken token = peek();
    has_peek_ = false;
    return token;
  }

private:
  void advance()
  {
    if(pos_ >= text_.size()) {
      return;
    }
    if(text_[pos_] == '\n') {
      ++line_;
      column_ = 1;
    } else {
      ++column_;
    }
    ++pos_;
  }

  void skip_space_and_comments()
  {
    while(pos_ < text_.size()) {
      const char ch = text_[pos_];
      if(std::isspace(static_cast<unsigned char>(ch)) != 0) {
        advance();
        continue;
      }
      if(ch == '#') {
        while(pos_ < text_.size() && text_[pos_] != '\n') {
          advance();
        }
        continue;
      }
      break;
    }
  }

  LowirToken read_token()
  {
    skip_space_and_comments();
    LowirToken token;
    token.line = line_;
    token.column = column_;
    if(pos_ == text_.size()) {
      token.text = "<eof>";
      return token;
    }

    const char ch = text_[pos_];
    if(ch == '-' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '>') {
      advance();
      advance();
      token.text = "->";
      return token;
    }
    if(ch == '!' && text_.compare(pos_, 4, "!dbg") == 0) {
      for(std::size_t i = 0; i < 4; ++i) {
        advance();
      }
      token.text = "!dbg";
      return token;
    }
    if(ch == '(' || ch == ')' || ch == '{' || ch == '}' ||
       ch == '[' || ch == ']' || ch == ':' || ch == ',' ||
       ch == '=' || ch == '<' || ch == '>' || ch == '+') {
      advance();
      token.text.assign(1, ch);
      return token;
    }

    const std::size_t start = pos_;
    while(pos_ < text_.size() && !is_delimiter(text_[pos_])) {
      if(text_[pos_] == '-' && pos_ + 1 < text_.size() &&
         text_[pos_ + 1] == '>') {
        break;
      }
      advance();
    }
    if(pos_ == start) {
      advance();
      token.text.assign(1, ch);
    } else {
      token.text = text_.substr(start, pos_ - start);
    }
    return token;
  }

  const std::string & text_;
  std::string source_name_;
  std::size_t pos_ = 0;
  std::size_t line_ = 1;
  std::size_t column_ = 1;
  bool has_peek_ = false;
  LowirToken peek_token_;
};

class LowirParser
{
public:
  LowirParser(const std::string & text, const std::string & source_name)
    : lexer_(text, source_name), source_name_(source_name)
  {}

  LowirProgram parse()
  {
    LowirProgram program;
    while(!at("<eof>")) {
      if(at("declare")) {
        parse_declaration(program);
      } else if(at("global")) {
        program.globals.push_back(parse_global_definition());
      } else if(at("function")) {
        program.functions.push_back(parse_function_definition());
      } else if(at("alias")) {
        program.object_aliases.push_back(parse_object_alias());
      } else {
        fail("expected a top-level LowIR item");
      }
    }
    return program;
  }

private:
  struct MetadataItem
  {
    std::string key;
    std::string value;
  };

  struct FunctionHeader
  {
    std::string name;
    std::vector<Parameter> params;
    LowType return_type;
    FunctionBoundaryMetadata boundary;
    SymbolMetadata metadata;
    InstructionDebugLocation debug_location;
  };

  bool at(const std::string & text)
  {
    return lexer_.peek().text == text;
  }

  LowirToken take()
  {
    return lexer_.take();
  }

  void fail(const std::string & message)
  {
    const LowirToken token = lexer_.peek();
    std::ostringstream out;
    out << source_name_ << ":" << token.line << ":" << token.column << ": "
        << message;
    throw ParseError(out.str());
  }

  LowirToken expect(const std::string & text)
  {
    if(!at(text)) {
      fail("expected '" + text + "'");
    }
    return take();
  }

  std::string parse_prefixed_name(char prefix, const std::string & what)
  {
    const LowirToken token = take();
    if(!has_prefix(token.text, prefix)) {
      fail("expected " + what);
    }
    return token.text;
  }

  std::string parse_global_name()
  {
    return parse_prefixed_name('@', "a global/function name");
  }

  std::string parse_function_name()
  {
    return parse_prefixed_name('@', "a function name");
  }

  std::string parse_temporary_name()
  {
    return parse_prefixed_name('%', "a temporary name");
  }

  std::string parse_slot_name()
  {
    return parse_prefixed_name('$', "a slot name");
  }

  std::string parse_block_name()
  {
    return parse_prefixed_name('^', "a block name");
  }

  std::string parse_object_symbol()
  {
    const LowirToken token = take();
    if(token.text.empty() || has_prefix(token.text, '@') ||
       has_prefix(token.text, '%') || has_prefix(token.text, '$') ||
       has_prefix(token.text, '^')) {
      fail("expected an object symbol");
    }
    return token.text;
  }

  long long parse_integer_token(const LowirToken & token)
  {
    if(token.text.empty()) {
      fail("expected an integer literal");
    }
    const char * begin = token.text.c_str();
    char * end = 0;
    errno = 0;
    const int base = token.text.size() > 2 && token.text[0] == '0' &&
                     (token.text[1] == 'x' || token.text[1] == 'X') ? 16 : 10;
    const long long value = std::strtoll(begin, &end, base);
    if(errno != 0 || end == begin || *end != '\0') {
      fail("expected an integer literal");
    }
    return value;
  }

  long long parse_integer_literal()
  {
    return parse_integer_token(take());
  }

  long double parse_float_token(const LowirToken & token)
  {
    std::string text = token.text;
    if(!text.empty() && (text[text.size() - 1] == 'f' ||
                         text[text.size() - 1] == 'F' ||
                         text[text.size() - 1] == 'l' ||
                         text[text.size() - 1] == 'L')) {
      text.erase(text.size() - 1);
    }
    char * end = 0;
    errno = 0;
    const long double value = std::strtold(text.c_str(), &end);
    if(errno != 0 || end == text.c_str() || *end != '\0') {
      fail("expected a floating-point literal");
    }
    return value;
  }

  LowType parse_type()
  {
    const std::string spelling = take().text;
    LowType type;
    if(spelling == "obj") {
      expect("<");
      const std::string bytes = take().text;
      std::string body = bytes;
      if(body.find('x') == std::string::npos) {
        expect("x");
        body += "x" + take().text;
      }
      expect(">");
      type.text = "obj<" + body + ">";
    } else {
      type.text = spelling;
    }
    if(!describe_low_type(type).valid()) {
      fail("unknown or malformed LowIR type");
    }
    return type;
  }

  Operand parse_scalar_literal(const LowType & type)
  {
    const LowTypeInfo info = describe_low_type(type);
    const LowirToken token = take();
    Operand operand;
    operand.text = token.text;
    operand.literal_type = type;
    if(info.floating()) {
      if(!looks_like_float(token.text)) {
        fail("expected a floating-point literal");
      }
      operand.kind = Operand::OP_FLOAT;
      operand.float_value = parse_float_token(token);
    } else if(info.integer()) {
      operand.kind = Operand::OP_INTEGER;
      operand.int_value = parse_integer_token(token);
    } else if(info.pointer() && token.text == "nullptr") {
      operand.kind = Operand::OP_INTEGER;
      operand.int_value = 0;
    } else {
      fail("type does not accept a scalar literal");
    }
    return operand;
  }

  Operand parse_value()
  {
    const LowirToken token = lexer_.peek();
    if(has_prefix(token.text, '%')) {
      Operand operand;
      operand.kind = Operand::OP_TEMP;
      operand.text = take().text;
      return operand;
    }
    if(has_prefix(token.text, '$')) {
      Operand operand;
      operand.kind = Operand::OP_SLOT;
      operand.text = take().text;
      return operand;
    }
    if(has_prefix(token.text, '@')) {
      Operand operand;
      operand.kind = Operand::OP_GLOBAL;
      operand.text = take().text;
      return operand;
    }
    if(has_prefix(token.text, '^')) {
      Operand operand;
      operand.kind = Operand::OP_LABEL;
      operand.text = take().text;
      return operand;
    }

    Operand operand;
    operand.text = take().text;
    if(operand.text == "nullptr") {
      operand.kind = Operand::OP_INTEGER;
      operand.int_value = 0;
    } else if(looks_like_float(operand.text)) {
      operand.kind = Operand::OP_FLOAT;
      operand.float_value = parse_float_token(token);
    } else {
      operand.kind = Operand::OP_INTEGER;
      operand.int_value = parse_integer_token(token);
    }
    return operand;
  }

  Operand parse_addressable()
  {
    const LowirToken token = lexer_.peek();
    if(!has_prefix(token.text, '@') && !has_prefix(token.text, '$')) {
      fail("expected an addressable global or slot");
    }
    return parse_value();
  }

  std::vector<MetadataItem> parse_metadata_blocks()
  {
    std::vector<MetadataItem> items;
    while(at("[")) {
      take();
      if(at("]")) {
        fail("metadata brackets cannot be empty");
      }
      do {
        MetadataItem item;
        item.key = take().text;
        if(item.key.empty() || has_prefix(item.key, '@') ||
           has_prefix(item.key, '%') || has_prefix(item.key, '$') ||
           has_prefix(item.key, '^')) {
          fail("expected a metadata key");
        }
        expect("=");
        item.value = take().text;
        if(item.value.empty() || item.value == "<eof>" ||
           item.value == "]" || item.value == ",") {
          fail("expected a metadata value");
        }
        items.push_back(item);
      } while(accept(","));
      expect("]");
    }
    return items;
  }

  bool accept(const std::string & text)
  {
    if(!at(text)) {
      return false;
    }
    take();
    return true;
  }

  bool yes_or_no(const std::string & value, bool & result)
  {
    if(value == "yes") {
      result = true;
      return true;
    }
    if(value == "no") {
      result = false;
      return true;
    }
    return false;
  }

  void apply_symbol_metadata(const MetadataItem & item, SymbolMetadata & metadata)
  {
    if(item.key == "role") {
      const std::string values[] = {
        "entry", "init", "fini", "eh_top", "eh_value", "eh_type",
        "eh_unhandled", "eh_allocate_exception", "eh_begin_catch",
        "eh_call_unexpected", "eh_current_exception_type", "eh_end_catch",
        "eh_rethrow", "eh_throw", "eh_personality", "eh_resume"
      };
      const SymbolRole roles[] = {
        SR_ENTRY, SR_INIT, SR_FINI, SR_EH_TOP, SR_EH_VALUE, SR_EH_TYPE,
        SR_EH_UNHANDLED, SR_EH_ALLOCATE_EXCEPTION, SR_EH_BEGIN_CATCH,
        SR_EH_CALL_UNEXPECTED, SR_EH_CURRENT_EXCEPTION_TYPE, SR_EH_END_CATCH,
        SR_EH_RETHROW, SR_EH_THROW, SR_EH_PERSONALITY, SR_EH_RESUME
      };
      for(std::size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        if(item.value == values[i]) {
          metadata.role = roles[i];
          return;
        }
      }
      fail("unknown symbol role");
    }
    if(item.key == "linkage") {
      if(item.value == "default") metadata.linkage = LLM_DEFAULT;
      else if(item.value == "c") metadata.linkage = LLM_C;
      else if(item.value == "cpp") metadata.linkage = LLM_CPP;
      else fail("unknown linkage metadata value");
      return;
    }
    if(item.key == "binding") {
      if(item.value == "default") metadata.binding = SBM_DEFAULT;
      else if(item.value == "internal") metadata.binding = SBM_INTERNAL;
      else if(item.value == "strong") metadata.binding = SBM_STRONG;
      else if(item.value == "weak") metadata.binding = SBM_WEAK;
      else fail("unknown binding metadata value");
      return;
    }
    if(item.key == "object") {
      if(item.value.empty() || has_prefix(item.value, '@')) {
        fail("invalid object metadata value");
      }
      metadata.object_symbol = item.value;
      return;
    }
    if(item.key == "tls_for") {
      if(!has_prefix(item.value, '@')) {
        fail("tls_for must name a global");
      }
      metadata.tls_for_symbol = item.value;
      return;
    }
    if(item.key == "keep_alias") {
      if(!yes_or_no(item.value, metadata.keep_internal_alias)) {
        fail("keep_alias must be yes or no");
      }
      return;
    }
    if(item.key == "prefer_local" || item.key == "prefer_local_object_binding") {
      if(!yes_or_no(item.value, metadata.prefer_local_object_binding)) {
        fail("prefer_local must be yes or no");
      }
      return;
    }
    if(item.key == "object_root" || item.key == "object_output_root") {
      if(!yes_or_no(item.value, metadata.object_output_root)) {
        fail("object_root must be yes or no");
      }
      return;
    }
    if(item.key == "trivial_lifecycle" || item.key == "object_trivial_lifecycle") {
      if(!yes_or_no(item.value, metadata.object_trivial_lifecycle)) {
        fail("trivial_lifecycle must be yes or no");
      }
      return;
    }
    if(item.key == "force_inline") {
      if(!yes_or_no(item.value, metadata.force_inline)) {
        fail("force_inline must be yes or no");
      }
      return;
    }
    fail("unknown symbol metadata key");
  }

  void apply_boundary_metadata(const MetadataItem & item,
                               FunctionBoundaryMetadata & boundary)
  {
    if(item.key == "arity") {
      if(item.value == "fixed") boundary.arity = CAM_FIXED;
      else if(item.value == "variadic") boundary.arity = CAM_VARIADIC;
      else if(item.value == "prototype_relaxed") boundary.arity = CAM_PROTOTYPE_RELAXED;
      else fail("unknown function arity metadata value");
      return;
    }
    if(item.key == "effects") {
      if(item.value == "readnone") boundary.effects = CFXM_READNONE;
      else if(item.value == "readonly") boundary.effects = CFXM_READONLY;
      else if(item.value == "readwrite") boundary.effects = CFXM_READWRITE;
      else fail("unknown function effects metadata value");
      return;
    }
    if(item.key == "unwind") {
      if(item.value == "may") boundary.unwind = CUM_MAY;
      else if(item.value == "no") boundary.unwind = CUM_NO;
      else fail("unknown function unwind metadata value");
      return;
    }
    if(item.key == "return") {
      if(item.value == "returns") boundary.returns = CRM_RETURNS;
      else if(item.value == "noreturn") boundary.returns = CRM_NORETURN;
      else fail("unknown function return metadata value");
      return;
    }
    fail("expected call-boundary metadata");
  }

  void apply_function_metadata(const std::vector<MetadataItem> & items,
                               FunctionBoundaryMetadata & boundary,
                               SymbolMetadata & metadata)
  {
    std::set<std::string> seen;
    for(std::size_t i = 0; i < items.size(); ++i) {
      if(!seen.insert(items[i].key).second) {
        fail("duplicate metadata key");
      }
      if(is_boundary_key(items[i].key)) {
        apply_boundary_metadata(items[i], boundary);
      } else {
        apply_symbol_metadata(items[i], metadata);
      }
    }
  }

  void apply_global_metadata(const std::vector<MetadataItem> & items,
                             GlobalStorageMode & storage,
                             SymbolMetadata & metadata)
  {
    std::set<std::string> seen;
    for(std::size_t i = 0; i < items.size(); ++i) {
      if(!seen.insert(items[i].key).second) {
        fail("duplicate metadata key");
      }
      if(items[i].key == "storage") {
        set_storage(items[i].value, storage);
      } else {
        apply_symbol_metadata(items[i], metadata);
      }
    }
  }

  void set_storage(const std::string & value, GlobalStorageMode & storage)
  {
    if(value == "default") storage = GSM_DEFAULT;
    else if(value == "writable") storage = GSM_WRITABLE;
    else if(value == "readonly") storage = GSM_READONLY;
    else if(value == "thread_local") storage = GSM_THREAD_LOCAL;
    else fail("unknown global storage metadata value");
  }

  void apply_parameter_metadata(const std::vector<MetadataItem> & items,
                                ParameterMetadata & metadata)
  {
    std::set<std::string> seen;
    for(std::size_t i = 0; i < items.size(); ++i) {
      if(!seen.insert(items[i].key).second) {
        fail("duplicate parameter metadata key");
      }
      if(items[i].key == "pass") {
        if(items[i].value == "direct") metadata.passing = PPM_DIRECT;
        else if(items[i].value == "indirect_result") metadata.passing = PPM_INDIRECT_RESULT;
        else if(items[i].value == "by_address") metadata.passing = PPM_BY_ADDRESS;
        else if(items[i].value == "reference") metadata.passing = PPM_REFERENCE;
        else if(items[i].value == "decay") metadata.passing = PPM_DECAY;
        else fail("unknown parameter pass metadata value");
      } else if(items[i].key == "capture") {
        if(items[i].value == "nocapture") metadata.capture = PCM_NOCAPTURE;
        else if(items[i].value == "maycapture") metadata.capture = PCM_MAYCAPTURE;
        else fail("unknown parameter capture metadata value");
      } else if(items[i].key == "access") {
        if(items[i].value == "none") metadata.access = PAM_NONE;
        else if(items[i].value == "read") metadata.access = PAM_READ;
        else if(items[i].value == "write") metadata.access = PAM_WRITE;
        else if(items[i].value == "readwrite") metadata.access = PAM_READWRITE;
        else fail("unknown parameter access metadata value");
      } else if(items[i].key == "alias") {
        if(items[i].value == "noalias") metadata.alias = PALM_NOALIAS;
        else fail("unknown parameter alias metadata value");
      } else {
        fail("unknown parameter metadata key");
      }
    }
  }

  IndexProjectionKind parse_index_metadata()
  {
    const std::vector<MetadataItem> items = parse_metadata_blocks();
    IndexProjectionKind projection = IPK_NONE;
    std::set<std::string> seen;
    for(std::size_t i = 0; i < items.size(); ++i) {
      if(!seen.insert(items[i].key).second || items[i].key != "projection") {
        fail("unknown index metadata");
      }
      if(items[i].value == "array_element") projection = IPK_ARRAY_ELEMENT;
      else if(items[i].value == "field") projection = IPK_FIELD;
      else if(items[i].value == "base_subobject") projection = IPK_BASE_SUBOBJECT;
      else if(items[i].value == "reference_field") projection = IPK_REFERENCE_FIELD;
      else fail("unknown index projection metadata value");
    }
    return projection;
  }

  Parameter parse_parameter()
  {
    Parameter parameter;
    parameter.name = parse_temporary_name();
    expect(":");
    parameter.type = parse_type();
    apply_parameter_metadata(parse_metadata_blocks(), parameter.metadata);
    return parameter;
  }

  std::vector<Parameter> parse_parameter_list()
  {
    std::vector<Parameter> params;
    if(at(")")) {
      return params;
    }
    params.push_back(parse_parameter());
    while(accept(",")) {
      params.push_back(parse_parameter());
    }
    return params;
  }

  InstructionDebugLocation parse_debug_location()
  {
    InstructionDebugLocation location;
    if(!accept("!dbg")) {
      return location;
    }
    expect("(");
    location.file = take().text;
    expect(",");
    const long long line = parse_integer_literal();
    expect(",");
    const long long column = parse_integer_literal();
    expect(")");
    if(line > 0 && static_cast<unsigned long long>(line) <=
                    static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
      location.line = static_cast<std::size_t>(line);
    }
    if(column > 0 && static_cast<unsigned long long>(column) <=
                       static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
      location.column = static_cast<std::size_t>(column);
    }
    return location;
  }

  FunctionHeader parse_function_header()
  {
    FunctionHeader header;
    header.name = parse_function_name();
    expect("(");
    header.params = parse_parameter_list();
    expect(")");
    expect("->");
    header.return_type = parse_type();
    apply_function_metadata(parse_metadata_blocks(), header.boundary, header.metadata);
    header.debug_location = parse_debug_location();
    return header;
  }

  void parse_declaration(LowirProgram & program)
  {
    expect("declare");
    if(at("global")) {
      program.global_declarations.push_back(parse_global_declaration());
    } else if(at("function")) {
      program.function_declarations.push_back(parse_function_declaration());
    } else {
      fail("expected global or function after declare");
    }
  }

  GlobalDeclaration parse_global_declaration()
  {
    GlobalDeclaration declaration;
    expect("global");
    declaration.name = parse_global_name();
    if(accept(":")) {
      declaration.has_type = true;
      declaration.type = parse_type();
    }
    apply_global_metadata(parse_metadata_blocks(), declaration.storage,
                          declaration.metadata);
    return declaration;
  }

  FunctionDeclaration parse_function_declaration()
  {
    FunctionDeclaration declaration;
    expect("function");
    const FunctionHeader header = parse_function_header();
    declaration.name = header.name;
    declaration.params = header.params;
    declaration.return_type = header.return_type;
    declaration.boundary = header.boundary;
    declaration.metadata = header.metadata;
    return declaration;
  }

  void parse_bare_storage(GlobalStorageMode & storage)
  {
    if(at("readonly")) {
      take();
      set_storage("readonly", storage);
    } else if(at("thread_local")) {
      take();
      set_storage("thread_local", storage);
    }
  }

  GlobalDefinition parse_global_definition()
  {
    GlobalDefinition global;
    expect("global");
    global.name = parse_global_name();
    parse_bare_storage(global.storage);
    if(at(":")) {
      take();
      global.type = parse_type();
      apply_global_metadata(parse_metadata_blocks(), global.storage, global.metadata);
      expect("=");
      global.init_kind = parse_global_initializer(global);
      return global;
    }

    apply_global_metadata(parse_metadata_blocks(), global.storage, global.metadata);
    expect("=");
    expect("{");
    global.structured = true;
    if(at("}")) {
      fail("structured globals need at least one data item");
    }
    while(!at("}")) {
      global.data_items.push_back(parse_global_data_item());
    }
    expect("}");
    return global;
  }

  GlobalDefinition::InitKind parse_global_initializer(GlobalDefinition & global)
  {
    if(accept("zero")) {
      return GlobalDefinition::INIT_ZERO;
    }
    if(accept("addr")) {
      global.init_operand = parse_value();
      if(global.init_operand.kind != Operand::OP_GLOBAL) {
        fail("global address initializer needs a global/function name");
      }
      global.addr_addend = parse_optional_addend();
      return GlobalDefinition::INIT_ADDR;
    }
    global.init_operand = parse_scalar_literal(global.type);
    return GlobalDefinition::INIT_INTEGER;
  }

  long long parse_optional_addend()
  {
    if(accept("+")) {
      return parse_integer_literal();
    }
    if(accept("-")) {
      return -parse_integer_literal();
    }
    return 0;
  }

  GlobalDefinition::DataItem parse_global_data_item()
  {
    GlobalDefinition::DataItem item;
    if(accept("zero")) {
      const long long bytes = parse_integer_literal();
      item.kind = GlobalDefinition::DataItem::ITEM_ZERO;
      if(bytes > 0) {
        item.zero_bytes = static_cast<std::size_t>(bytes);
      }
      return item;
    }

    item.type = parse_type();
    if(item.type.text == "ptr") {
      expect("addr");
      item.kind = GlobalDefinition::DataItem::ITEM_ADDR;
      item.symbol = parse_global_name();
      item.addr_addend = parse_optional_addend();
    } else {
      item.kind = GlobalDefinition::DataItem::ITEM_INTEGER;
      item.literal_operand = parse_scalar_literal(item.type);
    }
    return item;
  }

  Function parse_function_definition()
  {
    Function function;
    expect("function");
    const FunctionHeader header = parse_function_header();
    function.name = header.name;
    function.params = header.params;
    function.return_type = header.return_type;
    function.boundary = header.boundary;
    function.metadata = header.metadata;
    function.debug_location = header.debug_location;
    expect("{");
    while(!at("}")) {
      if(at("slot")) {
        function.slots.push_back(parse_slot_declaration());
      } else if(at("block")) {
        function.blocks.push_back(parse_block());
      } else if(at("<eof>")) {
        fail("unterminated function body");
      } else {
        fail("expected slot or block in function body");
      }
    }
    expect("}");
    return function;
  }

  std::pair<std::string, LowType> parse_slot_declaration()
  {
    expect("slot");
    const std::string name = parse_slot_name();
    expect(":");
    return std::make_pair(name, parse_type());
  }

  Block parse_block()
  {
    Block block;
    expect("block");
    block.label = parse_block_name();
    expect(":");
    while(!at("block") && !at("slot") && !at("}")) {
      if(at("<eof>")) {
        fail("unterminated block");
      }
      block.instructions.push_back(parse_instruction());
    }
    return block;
  }

  long long parse_memory_order()
  {
    const long long order = parse_integer_literal();
    if(order < 0 || order > 5) {
      fail("invalid atomic memory order");
    }
    return order;
  }

  std::pair<std::size_t, std::size_t> parse_span()
  {
    const std::string first = take().text;
    const std::size_t x = first.find('x');
    std::string bytes = first;
    std::string alignment;
    if(x == std::string::npos) {
      expect("x");
      alignment = take().text;
    } else {
      bytes = first.substr(0, x);
      alignment = first.substr(x + 1);
    }
    LowirToken byte_token;
    byte_token.text = bytes;
    LowirToken alignment_token;
    alignment_token.text = alignment;
    const long long byte_count = parse_integer_token(byte_token);
    const long long byte_alignment = parse_integer_token(alignment_token);
    return std::make_pair(byte_count > 0 ? static_cast<std::size_t>(byte_count) : 0,
                          byte_alignment > 0 ? static_cast<std::size_t>(byte_alignment) : 0);
  }

  Instruction parse_instruction()
  {
    Instruction instruction;
    if(has_prefix(lexer_.peek().text, '%')) {
      instruction.dest = parse_temporary_name();
      expect("=");
      instruction = parse_assigned_instruction(instruction.dest);
    } else {
      instruction = parse_standalone_instruction();
    }
    instruction.debug_location = parse_debug_location();
    return instruction;
  }

  Instruction parse_assigned_instruction(const std::string & dest)
  {
    const std::string opcode = take().text;
    Instruction instruction;
    instruction.dest = dest;
    if(opcode == "const") return parse_const(instruction);
    if(opcode == "copy") return parse_copy(instruction);
    if(opcode == "addr") return parse_addr(instruction);
    if(opcode == "load") return parse_load(instruction, Instruction::IK_LOAD);
    if(opcode == "atomic_load") return parse_load(instruction, Instruction::IK_ATOMIC_LOAD);
    if(opcode == "index") return parse_index(instruction);
    if(opcode == "unary") return parse_unary(instruction);
    if(opcode == "binary") return parse_binary(instruction);
    if(opcode == "cmp") return parse_cmp(instruction);
    if(opcode == "convert") return parse_convert(instruction);
    if(opcode == "atomic_add_fetch") return parse_atomic_add_fetch(instruction);
    if(opcode == "atomic_exchange") return parse_atomic_exchange(instruction);
    if(opcode == "atomic_compare_exchange") return parse_atomic_compare_exchange(instruction);
    if(opcode == "call") return parse_call(instruction, true);
    if(opcode == "exception") return parse_exception(instruction, Instruction::IK_EXCEPTION);
    if(opcode == "exception_selector") {
      return parse_exception(instruction, Instruction::IK_EXCEPTION_SELECTOR);
    }
    if(opcode == "stack_alloc") return parse_stack_alloc(instruction);
    if(opcode == "va_start") return parse_va_start(instruction);
    if(opcode == "va_arg") return parse_va_arg(instruction);
    fail("unknown assigned LowIR instruction");
    return instruction;
  }

  Instruction parse_standalone_instruction()
  {
    const std::string opcode = take().text;
    Instruction instruction;
    if(opcode == "store") return parse_store(instruction, Instruction::IK_STORE);
    if(opcode == "atomic_store") return parse_atomic_store(instruction);
    if(opcode == "atomic_thread_fence") return parse_fence(instruction, true);
    if(opcode == "atomic_signal_fence") return parse_fence(instruction, false);
    if(opcode == "call") return parse_call(instruction, false);
    if(opcode == "copyobj") return parse_bulk(instruction, Instruction::IK_COPYOBJ);
    if(opcode == "zeroinit") return parse_bulk(instruction, Instruction::IK_ZEROINIT);
    if(opcode == "eh_try") return parse_handler(instruction, Instruction::IK_EH_TRY);
    if(opcode == "eh_cleanup") return parse_handler(instruction, Instruction::IK_EH_CLEANUP);
    if(opcode == "eh_catch") return parse_eh_catch(instruction);
    if(opcode == "eh_filter") return parse_eh_filter(instruction);
    if(opcode == "eh_catch_all") { instruction.kind = Instruction::IK_EH_CATCH_ALL; return instruction; }
    if(opcode == "eh_end") { instruction.kind = Instruction::IK_EH_END; return instruction; }
    if(opcode == "throw") return parse_throw(instruction);
    if(opcode == "resume") { instruction.kind = Instruction::IK_RESUME; return instruction; }
    if(opcode == "jump") return parse_jump(instruction);
    if(opcode == "branch") return parse_branch(instruction);
    if(opcode == "switch") return parse_switch(instruction);
    if(opcode == "return") return parse_return(instruction);
    fail("unknown LowIR instruction");
    return instruction;
  }

  Instruction parse_const(Instruction instruction)
  {
    instruction.kind = Instruction::IK_CONST;
    instruction.type = parse_type();
    instruction.first = parse_scalar_literal(instruction.type);
    return instruction;
  }

  Instruction parse_copy(Instruction instruction)
  {
    instruction.kind = Instruction::IK_COPY;
    instruction.type = parse_type();
    instruction.first = parse_value();
    return instruction;
  }

  Instruction parse_addr(Instruction instruction)
  {
    instruction.kind = Instruction::IK_ADDR;
    instruction.type.text = "ptr";
    instruction.first = parse_addressable();
    return instruction;
  }

  Instruction parse_load(Instruction instruction, Instruction::Kind kind)
  {
    instruction.kind = kind;
    instruction.type = parse_type();
    instruction.first = parse_value();
    if(kind == Instruction::IK_ATOMIC_LOAD) {
      expect(",");
      parse_memory_order();
    }
    return instruction;
  }

  Instruction parse_index(Instruction instruction)
  {
    instruction.kind = Instruction::IK_INDEX;
    instruction.type = parse_type();
    instruction.index_projection = parse_index_metadata();
    instruction.first = parse_value();
    expect(",");
    instruction.second = parse_value();
    return instruction;
  }

  Instruction parse_unary(Instruction instruction)
  {
    instruction.kind = Instruction::IK_UNARY;
    instruction.op = take().text;
    instruction.type = parse_type();
    instruction.first = parse_value();
    return instruction;
  }

  Instruction parse_binary(Instruction instruction)
  {
    instruction.kind = Instruction::IK_BINARY;
    instruction.op = take().text;
    instruction.type = parse_type();
    instruction.first = parse_value();
    expect(",");
    instruction.second = parse_value();
    return instruction;
  }

  Instruction parse_cmp(Instruction instruction)
  {
    instruction.kind = Instruction::IK_CMP;
    instruction.op = take().text;
    instruction.type = parse_type();
    instruction.first = parse_value();
    expect(",");
    instruction.second = parse_value();
    return instruction;
  }

  Instruction parse_convert(Instruction instruction)
  {
    instruction.kind = Instruction::IK_CONVERT;
    instruction.op = take().text;
    instruction.type = parse_type();
    instruction.source_type = parse_type();
    instruction.first = parse_value();
    return instruction;
  }

  Instruction parse_atomic_add_fetch(Instruction instruction)
  {
    instruction.kind = Instruction::IK_ATOMIC_ADD_FETCH;
    instruction.type = parse_type();
    instruction.first = parse_value();
    expect(",");
    instruction.second = parse_value();
    expect(",");
    parse_memory_order();
    return instruction;
  }

  Instruction parse_atomic_exchange(Instruction instruction)
  {
    instruction.kind = Instruction::IK_ATOMIC_EXCHANGE;
    instruction.type = parse_type();
    instruction.first = parse_value();
    expect(",");
    instruction.second = parse_value();
    expect(",");
    parse_memory_order();
    return instruction;
  }

  Instruction parse_atomic_compare_exchange(Instruction instruction)
  {
    instruction.kind = Instruction::IK_ATOMIC_COMPARE_EXCHANGE;
    instruction.type = parse_type();
    instruction.first = parse_value();
    expect(",");
    instruction.second = parse_value();
    expect(",");
    instruction.third = parse_value();
    expect(",");
    parse_memory_order();
    expect(",");
    parse_memory_order();
    return instruction;
  }

  Instruction parse_exception(Instruction instruction, Instruction::Kind kind)
  {
    instruction.kind = kind;
    instruction.type = parse_type();
    return instruction;
  }

  Instruction parse_stack_alloc(Instruction instruction)
  {
    instruction.kind = Instruction::IK_STACK_ALLOC;
    const long long size = parse_integer_literal();
    instruction.byte_count = size > 0 ? static_cast<std::size_t>(size) : 0;
    instruction.type.text = "ptr";
    return instruction;
  }

  Instruction parse_va_start(Instruction instruction)
  {
    instruction.kind = Instruction::IK_VA_START;
    instruction.first = parse_value();
    return instruction;
  }

  Instruction parse_va_arg(Instruction instruction)
  {
    instruction.kind = Instruction::IK_VA_ARG;
    instruction.type = parse_type();
    instruction.first = parse_value();
    return instruction;
  }

  Instruction parse_store(Instruction instruction, Instruction::Kind kind)
  {
    instruction.kind = kind;
    instruction.type = parse_type();
    instruction.first = parse_value();
    expect(",");
    instruction.second = parse_value();
    return instruction;
  }

  Instruction parse_atomic_store(Instruction instruction)
  {
    instruction.kind = Instruction::IK_ATOMIC_STORE;
    instruction.type = parse_type();
    instruction.first = parse_value();
    expect(",");
    instruction.second = parse_value();
    expect(",");
    parse_memory_order();
    return instruction;
  }

  Instruction parse_fence(Instruction instruction, bool thread)
  {
    instruction.kind = thread ? Instruction::IK_ATOMIC_THREAD_FENCE :
                                Instruction::IK_ATOMIC_SIGNAL_FENCE;
    parse_memory_order();
    return instruction;
  }

  Instruction parse_call(Instruction instruction, bool assigned)
  {
    instruction.kind = Instruction::IK_CALL;
    instruction.type = parse_type();
    instruction.call_return_type = instruction.type;
    instruction.call_returns_void = instruction.type.text == "void";
    instruction.first = parse_value();
    expect("(");
    instruction.args = parse_argument_list();
    expect(")");
    if(accept("as")) {
      instruction.has_call_signature = true;
      expect("(");
      instruction.call_params = parse_parameter_list();
      expect(")");
      expect("->");
      instruction.call_return_type = parse_type();
      apply_call_metadata(parse_metadata_blocks(), instruction.call_boundary);
    }
    if(assigned && instruction.call_returns_void) {
      /* The validator reports the boundary error; retain the parsed shape. */
    }
    return instruction;
  }

  std::vector<Operand> parse_argument_list()
  {
    std::vector<Operand> args;
    if(at(")")) {
      return args;
    }
    args.push_back(parse_value());
    while(accept(",")) {
      args.push_back(parse_value());
    }
    return args;
  }

  void apply_call_metadata(const std::vector<MetadataItem> & items,
                           FunctionBoundaryMetadata & boundary)
  {
    std::set<std::string> seen;
    for(std::size_t i = 0; i < items.size(); ++i) {
      if(!seen.insert(items[i].key).second || !is_boundary_key(items[i].key)) {
        fail("call signatures accept only call-boundary metadata");
      }
      apply_boundary_metadata(items[i], boundary);
    }
  }

  Instruction parse_bulk(Instruction instruction, Instruction::Kind kind)
  {
    instruction.kind = kind;
    const std::pair<std::size_t, std::size_t> span = parse_span();
    instruction.byte_count = span.first;
    instruction.byte_alignment = span.second;
    instruction.first = parse_value();
    if(kind == Instruction::IK_COPYOBJ) {
      expect(",");
      instruction.second = parse_value();
    }
    return instruction;
  }

  Instruction parse_handler(Instruction instruction, Instruction::Kind kind)
  {
    instruction.kind = kind;
    instruction.first.kind = Operand::OP_LABEL;
    instruction.first.text = parse_block_name();
    return instruction;
  }

  Instruction parse_eh_catch(Instruction instruction)
  {
    instruction.kind = Instruction::IK_EH_CATCH;
    instruction.first.kind = Operand::OP_GLOBAL;
    instruction.first.text = parse_global_name();
    return instruction;
  }

  Instruction parse_eh_filter(Instruction instruction)
  {
    instruction.kind = Instruction::IK_EH_FILTER;
    if(has_prefix(lexer_.peek().text, '@')) {
      instruction.args.push_back(parse_value());
      while(accept(",")) {
        instruction.args.push_back(parse_value());
      }
    }
    return instruction;
  }

  Instruction parse_throw(Instruction instruction)
  {
    instruction.kind = Instruction::IK_THROW;
    instruction.type = parse_type();
    instruction.first = parse_value();
    return instruction;
  }

  Instruction parse_jump(Instruction instruction)
  {
    instruction.kind = Instruction::IK_JUMP;
    instruction.first.kind = Operand::OP_LABEL;
    instruction.first.text = parse_block_name();
    return instruction;
  }

  Instruction parse_branch(Instruction instruction)
  {
    instruction.kind = Instruction::IK_BRANCH;
    instruction.first = parse_value();
    expect(",");
    instruction.second.kind = Operand::OP_LABEL;
    instruction.second.text = parse_block_name();
    expect(",");
    instruction.third.kind = Operand::OP_LABEL;
    instruction.third.text = parse_block_name();
    return instruction;
  }

  Instruction parse_switch(Instruction instruction)
  {
    instruction.kind = Instruction::IK_SWITCH;
    instruction.first = parse_value();
    expect(",");
    instruction.second.kind = Operand::OP_LABEL;
    instruction.second.text = parse_block_name();
    while(accept(",")) {
      instruction.args.push_back(parse_value());
      expect(":");
      Operand label;
      label.kind = Operand::OP_LABEL;
      label.text = parse_block_name();
      instruction.args.push_back(label);
    }
    return instruction;
  }

  Instruction parse_return(Instruction instruction)
  {
    instruction.kind = Instruction::IK_RETURN;
    instruction.type = parse_type();
    if(instruction.type.text != "void" || value_starts_here()) {
      instruction.first = parse_value();
    }
    return instruction;
  }

  bool value_starts_here()
  {
    const std::string text = lexer_.peek().text;
    if(text == "nullptr" || has_prefix(text, '%') || has_prefix(text, '$') ||
       has_prefix(text, '@') || has_prefix(text, '^')) {
      return true;
    }
    if(text.empty()) {
      return false;
    }
    return std::isdigit(static_cast<unsigned char>(text[0])) != 0 ||
           ((text[0] == '-' || text[0] == '+') && text.size() > 1);
  }

  ObjectAlias parse_object_alias()
  {
    ObjectAlias alias;
    expect("alias");
    expect("object");
    alias.object_symbol = parse_object_symbol();
    expect("=");
    alias.target = parse_global_name();
    return alias;
  }

  LowirLexer lexer_;
  std::string source_name_;
};

namespace {

void append_program(LowirProgram & destination, LowirProgram && source)
{
  destination.global_declarations.insert(destination.global_declarations.end(),
    std::make_move_iterator(source.global_declarations.begin()),
    std::make_move_iterator(source.global_declarations.end()));
  destination.globals.insert(destination.globals.end(),
    std::make_move_iterator(source.globals.begin()),
    std::make_move_iterator(source.globals.end()));
  destination.function_declarations.insert(destination.function_declarations.end(),
    std::make_move_iterator(source.function_declarations.begin()),
    std::make_move_iterator(source.function_declarations.end()));
  destination.functions.insert(destination.functions.end(),
    std::make_move_iterator(source.functions.begin()),
    std::make_move_iterator(source.functions.end()));
  destination.object_aliases.insert(destination.object_aliases.end(),
    std::make_move_iterator(source.object_aliases.begin()),
    std::make_move_iterator(source.object_aliases.end()));
  destination.exported_symbols.insert(destination.exported_symbols.end(),
    std::make_move_iterator(source.exported_symbols.begin()),
    std::make_move_iterator(source.exported_symbols.end()));
}

std::string read_lowir_file(const std::string & path)
{
  std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
  if(!input) {
    throw ParseError("unable to read LowIR source '" + path + "'");
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  if(!input.good() && !input.eof()) {
    throw ParseError("unable to read LowIR source '" + path + "'");
  }
  return contents.str();
}

}  // namespace

LowirProgram parse_lowir_program_text(const std::string & text,
                                      const std::string & source_name)
{
  return LowirParser(text, source_name).parse();
}

LowirProgram parse_lowir_program_files(const std::vector<std::string> & paths)
{
  LowirProgram program;
  for(std::size_t i = 0; i < paths.size(); ++i) {
    LowirProgram parsed = parse_lowir_program_text(read_lowir_file(paths[i]), paths[i]);
    append_program(program, std::move(parsed));
  }
  return program;
}

}  // namespace lowir_model
