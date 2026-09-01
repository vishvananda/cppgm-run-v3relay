#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "posttoken_types.h"

struct IPPTokenStream;

struct IPostTokenOutputStream
{
	virtual void emit_invalid(const std::string& source) = 0;
	virtual void emit_simple(const std::string& source, ETokenType token_type) = 0;
	virtual void emit_identifier(const std::string& source) = 0;
	virtual void emit_literal(const std::string& source, EFundamentalType type,
		const void* data, std::size_t nbytes) = 0;
	virtual void emit_literal_array(const std::string& source,
		std::size_t num_elements, EFundamentalType type, const void* data,
		std::size_t nbytes) = 0;
	virtual void emit_user_defined_literal_character(const std::string& source,
		const std::string& ud_suffix, EFundamentalType type, const void* data,
		std::size_t nbytes) = 0;
	virtual void emit_user_defined_literal_string_array(
		const std::string& source, const std::string& ud_suffix,
		std::size_t num_elements, EFundamentalType type, const void* data,
		std::size_t nbytes) = 0;
	virtual void emit_user_defined_literal_integer(const std::string& source,
		const std::string& ud_suffix, const std::string& prefix) = 0;
	virtual void emit_user_defined_literal_floating(const std::string& source,
		const std::string& ud_suffix, const std::string& prefix) = 0;
	virtual void emit_eof() = 0;

	virtual ~IPostTokenOutputStream() {}
};

struct PostTokenStream : IPPTokenStream
{
	explicit PostTokenStream(IPostTokenOutputStream& output);

	void emit_whitespace_sequence() override;
	void emit_new_line() override;
	void emit_header_name(const std::string& data) override;
	void emit_identifier(const std::string& data) override;
	void emit_pp_number(const std::string& data) override;
	void emit_character_literal(const std::string& data) override;
	void emit_user_defined_character_literal(const std::string& data) override;
	void emit_string_literal(const std::string& data) override;
	void emit_user_defined_string_literal(const std::string& data) override;
	void emit_preprocessing_op_or_punc(const std::string& data) override;
	void emit_non_whitespace_char(const std::string& data) override;
	void emit_eof() override;

private:
	void append_string_token(const std::string& data, bool user_defined);
	void flush_string_sequence();
	void reset_string_sequence();

	IPostTokenOutputStream& output_;
	std::string pending_string_source_;
	std::string pending_string_prefix_;
	std::string pending_string_ud_suffix_;
	std::vector<std::uint32_t> pending_string_codepoints_;
	std::vector<unsigned char> pending_string_numeric_;
	std::size_t pending_string_token_count_;
	bool pending_string_sequence_active_;
	bool pending_string_invalid_;
	bool pending_string_has_ud_suffix_;
	bool pending_string_ud_suffix_valid_;
	bool pending_string_operator_candidate_;
	bool pending_string_empty_ordinary_;
	bool operator_pending_;
};
