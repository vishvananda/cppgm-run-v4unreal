#pragma once

#include <cstddef>
#include <memory>
#include <string>

struct IPPTokenStream
{
	// Spellings are borrowed callback data. A consumer that needs a token
	// after its callback returns must copy or otherwise retain its own facts.
	// Location-aware consumers may override this hook. It is called before
	// each spelling or new-line event and reports the physical source line of
	// that event; legacy PA1-PA4 consumers intentionally need no location state.
	virtual void set_source_file(const std::string& file) { (void)file; }
	virtual void retain_source_buffer(
		const std::string& file,
		const std::shared_ptr<const std::string>& source)
		{ (void)file; (void)source; }
	virtual void set_source_line(std::size_t line) { (void)line; }
	virtual void set_source_location(std::size_t line, std::size_t column)
		{ set_source_line(line); (void)column; }
	// Tokenizers with immutable byte offsets can provide a compact source
	// location. Legacy stages retain their existing line/column callbacks.
	virtual void set_source_position(std::size_t offset, std::size_t line,
					 std::size_t column)
		{ (void)offset; set_source_location(line, column); }
	virtual void emit_whitespace_sequence() = 0;
	virtual void emit_new_line() = 0;
	// Phase-4 consumers may treat physical newlines inside a comment as trivia
	// while still observing source locations. Older consumers retain the
	// ordinary new-line event by default.
	virtual void emit_comment_new_line() { emit_new_line(); }
	virtual void emit_header_name(const std::string& data) = 0;
	virtual void emit_identifier(const std::string& data) = 0;
	virtual void emit_pp_number(const std::string& data) = 0;
	virtual void emit_character_literal(const std::string& data) = 0;
	virtual void emit_user_defined_character_literal(const std::string& data) = 0;
	virtual void emit_string_literal(const std::string& data) = 0;
	virtual void emit_user_defined_string_literal(const std::string& data) = 0;
	virtual void emit_preprocessing_op_or_punc(const std::string& data) = 0;
	virtual void emit_non_whitespace_char(const std::string& data) = 0;
	virtual void emit_eof() = 0;

	virtual ~IPPTokenStream() {}
};
