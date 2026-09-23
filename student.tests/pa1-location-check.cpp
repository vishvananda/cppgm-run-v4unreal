#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "preprocess/tokens/PPTokenizer.h"

struct Event
{
  std::string type;
  std::string data;
  std::size_t line;
  std::size_t column;
};

class RecordingStream : public IPPTokenStream
{
public:
  RecordingStream() : line_(1), column_(1) {}
  void set_source_location(std::size_t line, std::size_t column)
  {
    line_ = line;
    column_ = column;
  }
  void emit_whitespace_sequence() { add("whitespace-sequence", ""); }
  void emit_new_line() { add("new-line", ""); }
  void emit_header_name(const std::string & s) { add("header-name", s); }
  void emit_identifier(const std::string & s) { add("identifier", s); }
  void emit_pp_number(const std::string & s) { add("pp-number", s); }
  void emit_character_literal(const std::string & s) { add("character-literal", s); }
  void emit_user_defined_character_literal(const std::string & s)
    { add("user-defined-character-literal", s); }
  void emit_string_literal(const std::string & s) { add("string-literal", s); }
  void emit_user_defined_string_literal(const std::string & s)
    { add("user-defined-string-literal", s); }
  void emit_preprocessing_op_or_punc(const std::string & s)
    { add("preprocessing-op-or-punc", s); }
  void emit_non_whitespace_char(const std::string & s)
    { add("non-whitespace-character", s); }
  void emit_eof() { add("eof", ""); }

  std::vector<Event> events;
private:
  std::size_t line_;
  std::size_t column_;
  void add(const std::string & type, const std::string & data)
  {
    Event event = { type, data, line_, column_ };
    events.push_back(event);
  }
};

int main()
{
  const std::string source = "a\\\nb /* c\n*/d\nR\"(x\ny)\"z\n";
  RecordingStream stream;
  PPTokenizer tokenizer(source, stream);
  tokenizer.tokenize();

  const char * expected[] = {
    "identifier|ab|1:1",
    "whitespace-sequence||2:2",
    "new-line||2:7",
    "identifier|d|3:3",
    "new-line||3:4",
    "user-defined-string-literal|R\"(x\ny)\"z|4:1",
    "new-line||5:5",
    "eof||5:5"
  };
  if (stream.events.size() != sizeof(expected) / sizeof(expected[0]))
    return 1;
  for (std::size_t i = 0; i < stream.events.size(); ++i)
  {
    const Event & event = stream.events[i];
    std::ostringstream actual;
    actual << event.type << '|' << event.data << '|'
           << event.line << ':' << event.column;
    if (actual.str() != expected[i])
    {
      std::cerr << "event " << i << " mismatch: " << actual.str()
                << " != " << expected[i] << '\n';
      return 1;
    }
  }

  // The physical quote follows a phase-2 splice. Its byte offset and source
  // location must be used when scanning the raw body and resuming afterward.
  const std::string spliced_prefix = "R\\\n\"(body)\"x\ny\n";
  RecordingStream spliced_stream;
  PPTokenizer spliced_tokenizer(spliced_prefix, spliced_stream);
  spliced_tokenizer.tokenize();
  const char * spliced_expected[] = {
    "user-defined-string-literal|R\"(body)\"x|1:1",
    "new-line||2:10",
    "identifier|y|3:1",
    "new-line||3:2",
    "eof||3:2"
  };
  if (spliced_stream.events.size() !=
      sizeof(spliced_expected) / sizeof(spliced_expected[0]))
    return 1;
  for (std::size_t i = 0; i < spliced_stream.events.size(); ++i)
  {
    const Event & event = spliced_stream.events[i];
    std::ostringstream actual;
    actual << event.type << '|' << event.data << '|'
           << event.line << ':' << event.column;
    if (actual.str() != spliced_expected[i])
    {
      std::cerr << "spliced event " << i << " mismatch: " << actual.str()
                << " != " << spliced_expected[i] << '\n';
      return 1;
    }
  }
  return 0;
}
