#include "ast_parser.h"

#include <limits>

using namespace std;

namespace
{

const size_t NoPosition = numeric_limits<size_t>::max();

bool IsWordToken(const Pa6Token& token)
{
	return token.kind == PA6_IDENTIFIER_TOKEN ||
		(token.kind == PA6_SIMPLE_TOKEN &&
		 token.simple_type >= KW_ALIGNAS && token.simple_type <= KW_WHILE);
}

} // namespace

Pa10Parser::Pa10Parser(const vector<Pa6Token>& tokens, AstArena& arena)
	: tokens_(tokens), arena_(arena), pos_(0)
{
}

AstId Pa10Parser::ParseTranslationUnit()
{
	return parse_translation_unit();
}

const Pa6Token& Pa10Parser::token(size_t at) const
{
	static const Pa6Token end_token(PA6_EOF_TOKEN, "");
	return at < tokens_.size() ? tokens_[at] : end_token;
}

bool Pa10Parser::at_end() const
{
	return token(pos_).kind == PA6_EOF_TOKEN;
}

bool Pa10Parser::is_simple(ETokenType type, size_t at) const
{
	return token(at == NoPosition ? pos_ : at).IsSimple(type);
}

bool Pa10Parser::is_kind(Pa6TokenKind kind, size_t at) const
{
	return token(at == NoPosition ? pos_ : at).kind == kind;
}

bool Pa10Parser::consume_simple(ETokenType type)
{
	if (!is_simple(type))
		return false;
	++pos_;
	return true;
}

bool Pa10Parser::enter_bracket(ETokenType type)
{
	if (!consume_simple(type))
		return false;
	switch (type)
	{
	case OP_LPAREN: brackets_.push_back(BRACKET_PAREN); break;
	case OP_LSQUARE: brackets_.push_back(BRACKET_SQUARE); break;
	case OP_LBRACE: brackets_.push_back(BRACKET_BRACE); break;
	default: return false;
	}
	return true;
}

bool Pa10Parser::leave_bracket(ETokenType type)
{
	if (!is_simple(type) || brackets_.empty())
		return false;
	const BracketKind wanted = type == OP_RPAREN ? BRACKET_PAREN :
		type == OP_RSQUARE ? BRACKET_SQUARE : BRACKET_BRACE;
	if (brackets_.back() != wanted)
		return false;
	++pos_;
	brackets_.pop_back();
	return true;
}

bool Pa10Parser::has_angle_boundary() const
{
	for (size_t i = brackets_.size(); i != 0; --i)
	{
		if (brackets_[i - 1] != BRACKET_ANGLE)
			continue;
		for (size_t j = i; j < brackets_.size(); ++j)
			if (brackets_[j] != BRACKET_ANGLE)
				return false;
		return true;
	}
	return false;
}

bool Pa10Parser::parse_close_angle_bracket()
{
	if (brackets_.empty() || brackets_.back() != BRACKET_ANGLE)
		return false;
	if (is_simple(OP_GT))
		++pos_;
	else if (is_kind(PA6_RSHIFT_1_TOKEN) || is_kind(PA6_RSHIFT_2_TOKEN))
		++pos_;
	else
		return false;
	brackets_.pop_back();
	return true;
}

Pa10Parser::Mark Pa10Parser::mark() const
{
	Mark result;
	result.position = pos_;
	result.brackets = brackets_.size();
	result.scope = scopes_.Mark();
	return result;
}

void Pa10Parser::restore(const Mark& saved)
{
	pos_ = saved.position;
	brackets_.resize(saved.brackets);
	if (scopes_.Rollback(saved.scope))
		memo_.clear();
}

uint64_t Pa10Parser::memo_key(unsigned rule) const
{
	return (static_cast<uint64_t>(rule) << 56) |
		(static_cast<uint64_t>(pos_) << 1) | (has_angle_boundary() ? 1u : 0u);
}

AstId Pa10Parser::try_memoized(unsigned rule, AstRule implementation)
{
	const uint64_t key = memo_key(rule);
	const unordered_map<uint64_t, MemoEntry>::const_iterator found =
		memo_.find(key);
	if (found != memo_.end())
	{
		if (found->second.node != 0)
			pos_ = found->second.end;
		return found->second.node;
	}
	const Mark saved = mark();
	const AstId node = (this->*implementation)();
	if (node == 0)
		restore(saved);
	MemoEntry entry;
	entry.node = node;
	entry.end = pos_;
	memo_[key] = entry;
	return node;
}

AstId Pa10Parser::make(AstKind kind)
{
	return arena_.Make(kind, string(), 0, 0);
}

AstId Pa10Parser::make_span(AstKind kind, size_t first, size_t last,
	const string& text)
{
	return arena_.Make(kind, text, first, last);
}

AstId Pa10Parser::make_join(AstKind kind, size_t first, size_t last)
{
	return arena_.Make(kind, Join(first, last), first, last);
}

AstId Pa10Parser::make_token(AstKind kind, size_t at)
{
	return arena_.Make(kind, token_label(token(at)), at, at + 1);
}

AstId Pa10Parser::make_operator_id(size_t first)
{
	// An unqualified operator-function-id prints as "operator" glued to the
	// spellings that follow it: operatorint, operator"" _x, operatornew[].
	return arena_.Make(AST_IDENTIFIER, string("operator") + Concat(first + 1, pos_),
		first, pos_);
}

void Pa10Parser::add(AstId parent, AstId child)
{
	arena_.Add(parent, child);
}

string Pa10Parser::token_label(const Pa6Token& tok) const
{
	if (tok.kind == PA6_IDENTIFIER_TOKEN)
		return "TT_IDENTIFIER:" + tok.spelling;
	if (tok.kind == PA6_LITERAL_TOKEN)
		return "TT_LITERAL:" + tok.spelling;
	if (tok.kind == PA6_RSHIFT_1_TOKEN || tok.kind == PA6_RSHIFT_2_TOKEN)
		return "OP_GT:>";
	map<ETokenType, string>::const_iterator found =
		TokenTypeToStringMap.find(tok.simple_type);
	if (found == TokenTypeToStringMap.end())
		return tok.spelling;
	return found->second + ":" + tok.spelling;
}

string Pa10Parser::Join(size_t first, size_t last) const
{
	string result;
	bool previous_word = false;
	for (size_t i = first; i < last; ++i)
	{
		const Pa6Token& current = token(i);
		if (current.kind == PA6_EOF_TOKEN)
			break;
		const bool current_word = IsWordToken(current);
		if (!result.empty() && previous_word && current_word)
			result += ' ';
		if (current.kind == PA6_RSHIFT_1_TOKEN ||
			current.kind == PA6_RSHIFT_2_TOKEN)
			result += '>';
		else
			result += current.spelling;
		previous_word = current_word;
	}
	return result;
}

string Pa10Parser::Concat(size_t first, size_t last) const
{
	string result;
	for (size_t i = first; i < last; ++i)
	{
		if (token(i).kind == PA6_RSHIFT_1_TOKEN ||
			token(i).kind == PA6_RSHIFT_2_TOKEN)
			result += '>';
		else
			result += token(i).spelling;
	}
	return result;
}

bool Pa10Parser::is_builtin_type(ETokenType type) const
{
	switch (type)
	{
	case KW_CHAR: case KW_CHAR16_T: case KW_CHAR32_T: case KW_WCHAR_T:
	case KW_BOOL: case KW_SHORT: case KW_INT: case KW_LONG: case KW_SIGNED:
	case KW_UNSIGNED: case KW_FLOAT: case KW_DOUBLE: case KW_VOID:
	case KW_AUTO:
		return true;
	default:
		return false;
	}
}

bool Pa10Parser::is_cv_qualifier(ETokenType type) const
{
	return type == KW_CONST || type == KW_VOLATILE;
}

bool Pa10Parser::is_storage_or_function_specifier(ETokenType type) const
{
	switch (type)
	{
	case KW_REGISTER: case KW_STATIC: case KW_THREAD_LOCAL: case KW_EXTERN:
	case KW_MUTABLE: case KW_INLINE: case KW_VIRTUAL: case KW_EXPLICIT:
	case KW_FRIEND: case KW_TYPEDEF: case KW_CONSTEXPR:
		return true;
	default:
		return false;
	}
}

bool Pa10Parser::is_keyword_literal(ETokenType type) const
{
	return type == KW_TRUE || type == KW_FALSE || type == KW_NULLPTR ||
		type == KW_THIS;
}

bool Pa10Parser::is_assignment_operator(ETokenType type) const
{
	switch (type)
	{
	case OP_ASS: case OP_STARASS: case OP_DIVASS: case OP_MODASS:
	case OP_PLUSASS: case OP_MINUSASS: case OP_RSHIFTASS: case OP_LSHIFTASS:
	case OP_BANDASS: case OP_XORASS: case OP_BORASS:
		return true;
	default:
		return false;
	}
}

bool Pa10Parser::can_start_expression() const
{
	const Pa6Token& current = token(pos_);
	if (current.kind == PA6_IDENTIFIER_TOKEN || current.kind == PA6_LITERAL_TOKEN)
		return true;
	if (current.kind == PA6_RSHIFT_1_TOKEN || current.kind == PA6_RSHIFT_2_TOKEN)
		return false;
	if (current.kind != PA6_SIMPLE_TOKEN)
		return false;
	if (is_builtin_type(current.simple_type) || is_keyword_literal(current.simple_type))
		return true;
	switch (current.simple_type)
	{
	case OP_LPAREN: case OP_LSQUARE: case OP_COLON2: case OP_PLUS:
	case OP_MINUS: case OP_STAR: case OP_AMP: case OP_LNOT: case OP_COMPL:
	case OP_INC: case OP_DEC: case KW_SIZEOF: case KW_ALIGNOF:
	case KW_NOEXCEPT: case KW_TYPEID: case KW_NEW: case KW_DELETE:
	case KW_STATIC_CAST: case KW_DYNAMIC_CAST: case KW_CONST_CAST:
	case KW_REINTERPET_CAST:
		return true;
	default:
		return false;
	}
}

bool Pa10Parser::can_start_declaration() const
{
	const Pa6Token& current = token(pos_);
	if (current.kind == PA6_IDENTIFIER_TOKEN)
	{
		const BindKind* binding = scopes_.Lookup(current.spelling);
		return binding == 0 || *binding == BIND_TYPE || *binding == BIND_TEMPLATE ||
			is_simple(OP_COLON2, pos_ + 1);
	}
	if (current.kind != PA6_SIMPLE_TOKEN)
		return false;
	return is_builtin_type(current.simple_type) ||
		is_cv_qualifier(current.simple_type) ||
		is_storage_or_function_specifier(current.simple_type) ||
		current.IsSimple(KW_DECLTYPE) || current.IsSimple(KW_TYPENAME) ||
		current.IsSimple(KW_STATIC_ASSERT) || current.IsSimple(KW_USING) ||
		current.IsSimple(KW_TEMPLATE) ||
		current.IsSimple(OP_COLON2) ||
		current.IsSimple(KW_CLASS) || current.IsSimple(KW_STRUCT) ||
		current.IsSimple(KW_UNION) || current.IsSimple(KW_ENUM) ||
		current.IsSimple(KW_NAMESPACE);
}

bool Pa10Parser::consume_attribute_specifiers()
{
	bool consumed = false;
	while (true)
	{
		if (is_simple(OP_LSQUARE) && is_simple(OP_LSQUARE, pos_ + 1))
		{
			const Mark attribute = mark();
			pos_ += 2;
			unsigned depth = 1;
			while (depth != 0)
			{
				if (at_end())
				{
					restore(attribute);
					return false;
				}
				if (is_simple(OP_LSQUARE) && is_simple(OP_LSQUARE, pos_ + 1))
				{
					++depth;
					pos_ += 2;
				}
				else if (is_simple(OP_RSQUARE) &&
					is_simple(OP_RSQUARE, pos_ + 1))
				{
					--depth;
					pos_ += 2;
				}
				else
					++pos_;
			}
			consumed = true;
			continue;
		}
		if ((is_kind(PA6_IDENTIFIER_TOKEN) &&
				(token(pos_).spelling == "__attribute__" ||
				 token(pos_).spelling == "__declspec")) ||
			is_simple(KW_ALIGNAS))
		{
			consumed = true;
			++pos_;
			if (!consume_simple(OP_LPAREN))
				return false;
			unsigned depth = 1;
			while (depth != 0)
			{
				if (at_end())
					return false;
				if (is_simple(OP_LPAREN))
				{
					++depth;
					++pos_;
				}
				else if (is_simple(OP_RPAREN))
				{
					--depth;
					++pos_;
				}
				else
					++pos_;
			}
			continue;
		}
		break;
	}
	return consumed;
}

bool Pa10Parser::node_has_kind(AstId node, AstKind kind) const
{
	if (node == 0)
		return false;
	const AstNode& value = arena_.At(node);
	if (value.kind == kind)
		return true;
	for (size_t i = 0; i < value.children.size(); ++i)
		if (node_has_kind(value.children[i], kind))
			return true;
	return false;
}

bool Pa10Parser::specifier_seq_has_keyword(AstId specifiers,
	ETokenType type) const
{
	const AstNode& sequence = arena_.At(specifiers);
	for (size_t i = 0; i < sequence.children.size(); ++i)
	{
		const AstNode& specifier = arena_.At(sequence.children[i]);
		if (specifier.kind == AST_DECL_SPECIFIER &&
			specifier.last == specifier.first + 1 &&
			token(specifier.first).IsSimple(type))
			return true;
	}
	return false;
}

// The declarator-id node of a declarator, looking through nested declarators
// such as (*name)(int).  Abstract declarators have none.
AstId Pa10Parser::declarator_identifier(AstId declarator) const
{
	while (declarator != 0)
	{
		const AstNode& node = arena_.At(declarator);
		AstId inner = 0;
		for (size_t i = 0; i < node.children.size() && inner == 0; ++i)
		{
			const AstKind kind = arena_.At(node.children[i]).kind;
			if (kind == AST_IDENTIFIER)
				return node.children[i];
			if (kind == AST_NESTED_DECLARATOR || kind == AST_DECLARATOR)
				inner = node.children[i];
		}
		declarator = inner;
	}
	return 0;
}

// The identifier a declarator-id introduces into the current scope.  A
// qualified name, template-id or operator name redeclares an entity that lives
// elsewhere and introduces nothing here.
string Pa10Parser::declared_identifier(AstId identifier) const
{
	if (identifier == 0)
		return string();
	const AstNode& node = arena_.At(identifier);
	if (node.last != node.first + 1 || !token(node.first).IsIdentifier())
		return string();
	return token(node.first).spelling;
}

// The unqualified name a template-declaration introduces: the last identifier
// of the declared name outside template-argument lists and parentheses.
// Operator and destructor names introduce no template name.
string Pa10Parser::template_name(AstId name) const
{
	if (name == 0)
		return string();
	const AstNode& node = arena_.At(name);
	string result;
	size_t angle_depth = 0;
	size_t group_depth = 0;
	for (size_t i = node.first; i < node.last; ++i)
	{
		const Pa6Token& current = token(i);
		if (current.IsSimple(OP_LPAREN) || current.IsSimple(OP_LSQUARE))
			++group_depth;
		else if (current.IsSimple(OP_RPAREN) || current.IsSimple(OP_RSQUARE))
			group_depth -= group_depth != 0;
		else if (group_depth != 0)
			continue;
		else if (current.IsSimple(OP_LT))
			++angle_depth;
		else if (current.IsSimple(OP_GT) || current.IsRshiftPart())
			angle_depth -= angle_depth != 0;
		else if (angle_depth != 0)
			continue;
		else if (current.IsSimple(KW_OPERATOR) || current.IsSimple(OP_COMPL))
			return string();
		else if (current.IsIdentifier())
			result = current.spelling;
	}
	return result;
}

void Pa10Parser::bind_declarator_name(AstId declarator, BindKind kind)
{
	scopes_.Bind(declared_identifier(declarator_identifier(declarator)), kind);
}

// Binds the name of every parameter-declaration below node as a value; used
// for the scope of a function, special member or lambda body.
void Pa10Parser::bind_parameters(AstId node)
{
	if (node == 0)
		return;
	const AstNode& value = arena_.At(node);
	if (value.kind == AST_PARAMETER_DECLARATION)
	{
		for (size_t i = 0; i < value.children.size(); ++i)
			if (arena_.At(value.children[i]).kind == AST_DECLARATOR)
				bind_declarator_name(value.children[i], BIND_VALUE);
		return;
	}
	for (size_t i = 0; i < value.children.size(); ++i)
		bind_parameters(value.children[i]);
}

void Pa10Parser::bind_template_declaration(AstId declaration)
{
	if (declaration == 0)
		return;
	const AstNode& node = arena_.At(declaration);
	AstId name = 0;
	switch (node.kind)
	{
	case AST_CLASS_SPECIFIER:
	case AST_CLASS_FORWARD_DECLARATION:
	case AST_ALIAS_DECLARATION:
		name = declaration;
		break;
	case AST_TEMPLATE_DECLARATION:
		bind_template_declaration(node.children.back());
		return;
	case AST_FUNCTION_DEFINITION:
	case AST_SIMPLE_DECLARATION:
	case AST_SPECIAL_MEMBER_DEFINITION:
	case AST_SPECIAL_MEMBER_DECLARATION:
		for (size_t i = 0; i < node.children.size() && name == 0; ++i)
		{
			const AstNode& child = arena_.At(node.children[i]);
			if (child.kind == AST_DECLARATOR)
				name = declarator_identifier(node.children[i]);
			else if (child.kind == AST_INIT_DECLARATOR_LIST)
				name = declarator_identifier(
					arena_.At(child.children[0]).children[0]);
		}
		break;
	default:
		return;
	}
	scopes_.Bind(template_name(name), BIND_TEMPLATE);
}

AstId Pa10Parser::parse_translation_unit()
{
	const Mark saved = mark();
	const AstId root = make(AST_TRANSLATION_UNIT);
	while (!at_end())
	{
		const AstId declaration = parse_declaration();
		if (declaration == 0)
		{
			restore(saved);
			return 0;
		}
		add(root, declaration);
	}
	return root;
}

AstId Pa10Parser::parse_declaration(bool member_context)
{
	const Mark saved = mark();
	if (is_simple(OP_SEMICOLON))
		return parse_empty_declaration();
	if (is_simple(KW_TEMPLATE) ||
		(is_simple(KW_EXTERN) && is_simple(KW_TEMPLATE, pos_ + 1)))
	{
		const AstId explicit_instantiation =
			parse_explicit_instantiation_declaration();
		if (explicit_instantiation != 0)
			return explicit_instantiation;
		restore(saved);
		const AstId templated = parse_template_declaration(member_context);
		if (templated != 0)
			return templated;
		restore(saved);
	}
	if (is_simple(KW_STATIC_ASSERT))
		return parse_static_assert_declaration();
	if (is_simple(KW_NAMESPACE))
	{
		const AstId namespace_alias = parse_namespace_alias_definition();
		if (namespace_alias != 0)
			return namespace_alias;
		restore(saved);
	}
	if (is_simple(KW_INLINE) || is_simple(KW_NAMESPACE))
	{
		const AstId namespace_definition = parse_namespace_definition();
		if (namespace_definition != 0)
			return namespace_definition;
		restore(saved);
	}
	if (is_simple(KW_EXTERN))
	{
		const AstId linkage = parse_linkage_specification();
		if (linkage != 0)
			return linkage;
		restore(saved);
	}
	if (is_simple(KW_USING))
	{
		const AstId alias = parse_alias_declaration();
		if (alias != 0)
			return alias;
		restore(saved);
		const AstId directive = parse_using_directive();
		if (directive != 0)
			return directive;
		restore(saved);
		const AstId declaration = parse_using_declaration();
		if (declaration != 0)
			return declaration;
		restore(saved);
	}
	const AstId special = parse_special_member_definition();
	if (special != 0)
		return special;
	restore(saved);
	return parse_specified_declaration(member_context);
}

AstId Pa10Parser::parse_empty_declaration()
{
	if (!consume_simple(OP_SEMICOLON))
		return 0;
	return make(AST_EMPTY_DECLARATION);
}

// A declaration that starts with a decl-specifier-seq: a function definition,
// a bit-field (in a class body), or an init-declarator-list.  The specifiers
// and the first declarator are parsed once and the continuation is chosen from
// the next token, so a class body inside the specifiers is never re-parsed.
AstId Pa10Parser::parse_specified_declaration(bool member_context)
{
	const Mark saved = mark();
	vector<AstId> alignments;
	while (true)
	{
		const AstId alignment = parse_alignment_specifier();
		if (alignment != 0)
		{
			alignments.push_back(alignment);
			continue;
		}
		if (consume_attribute_specifiers())
			continue;
		break;
	}
	const AstId specifiers = parse_decl_specifier_seq();
	if (specifiers == 0)
		return 0;
	if (consume_simple(OP_SEMICOLON))
	{
		// A lone class or enum definition or forward declaration prints as
		// the specifier itself.
		const vector<AstId>& children = arena_.At(specifiers).children;
		if (children.size() == 1)
		{
			const AstKind kind = arena_.At(children[0]).kind;
			if (kind == AST_CLASS_SPECIFIER ||
				kind == AST_CLASS_FORWARD_DECLARATION ||
				kind == AST_ENUM_SPECIFIER || kind == AST_ENUM_DECLARATION)
			{
				for (size_t i = 0; i < alignments.size(); ++i)
					add(children[0], alignments[i]);
				// An unnamed specifier has no name span; record the extent of
				// the declaration it forms so sema can give the type an identity.
				const AstNode& specifier = arena_.At(children[0]);
				if (specifier.first == specifier.last)
					arena_.RecordDeclarationExtent(children[0], saved.position,
						pos_);
				return children[0];
			}
		}
		const AstId result = make(AST_SIMPLE_DECLARATION);
		add(result, specifiers);
		for (size_t i = 0; i < alignments.size(); ++i)
			add(result, alignments[i]);
		return result;
	}
	AstId declarator = 0;
	if (!(member_context && is_simple(OP_COLON)))
	{
		declarator = parse_declarator();
		if (declarator == 0)
		{
			restore(saved);
			return 0;
		}
	}
	if (member_context && is_simple(OP_COLON))
	{
		const AstId result = finish_bit_field_declaration(saved, specifiers,
			declarator);
		if (result == 0)
			return 0;
		for (size_t i = 0; i < alignments.size(); ++i)
			add(result, alignments[i]);
		return result;
	}
	if (declarator != 0 && is_simple(OP_LBRACE) &&
		node_has_kind(declarator, AST_PARAMETER_CLAUSE))
	{
		const AstId definition =
			finish_function_definition(specifiers, declarator);
		if (definition != 0)
		{
			for (size_t i = 0; i < alignments.size(); ++i)
				add(definition, alignments[i]);
			return definition;
		}
	}
	const AstId result = finish_simple_declaration(saved, specifiers,
		declarator);
	if (result == 0)
		return 0;
	for (size_t i = 0; i < alignments.size(); ++i)
		add(result, alignments[i]);
	return result;
}

AstId Pa10Parser::finish_function_definition(AstId specifiers,
	AstId declarator)
{
	const Mark saved = mark();
	// The function name is visible while parsing its body, and parameter
	// bindings live only in the function scope.
	bind_declarator_name(declarator, BIND_VALUE);
	scopes_.Push();
	bind_parameters(declarator);
	const AstId body = parse_compound_statement();
	scopes_.Pop();
	if (body == 0)
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_FUNCTION_DEFINITION);
	add(result, specifiers);
	add(result, declarator);
	add(result, body);
	return result;
}

AstId Pa10Parser::finish_simple_declaration(const Mark& saved,
	AstId specifiers, AstId declarator)
{
	const AstId list = make(AST_INIT_DECLARATOR_LIST);
	AstId item = finish_init_declarator(declarator);
	while (item != 0)
	{
		add(list, item);
		if (!consume_simple(OP_COMMA))
			break;
		item = parse_init_declarator();
	}
	if (item == 0 || !consume_simple(OP_SEMICOLON))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_SIMPLE_DECLARATION);
	add(result, specifiers);
	add(result, list);
	const BindKind kind = specifier_seq_has_keyword(specifiers, KW_TYPEDEF) ?
		BIND_TYPE : BIND_VALUE;
	const vector<AstId>& items = arena_.At(list).children;
	for (size_t i = 0; i < items.size(); ++i)
		bind_declarator_name(arena_.At(items[i]).children[0], kind);
	return result;
}

AstId Pa10Parser::finish_bit_field_declaration(const Mark& saved,
	AstId specifiers, AstId declarator)
{
	++pos_;
	const AstId width = parse_assignment_expression();
	if (width == 0 || !consume_simple(OP_SEMICOLON))
	{
		restore(saved);
		return 0;
	}
	const AstId bit_declarator = make(AST_BIT_FIELD_DECLARATOR);
	add(bit_declarator, declarator);
	add(bit_declarator, width);
	const AstId result = make(AST_BIT_FIELD_DECLARATION);
	add(result, specifiers);
	add(result, bit_declarator);
	bind_declarator_name(declarator, BIND_VALUE);
	return result;
}

AstId Pa10Parser::parse_statement()
{
	if (is_simple(OP_LBRACE))
		return parse_compound_statement();
	if (is_simple(KW_IF) || is_simple(KW_WHILE) || is_simple(KW_FOR) ||
		is_simple(KW_SWITCH) || is_simple(KW_DO))
		return parse_selection_statement();
	if (is_simple(KW_TRY))
		return parse_try_block();
	if (is_simple(KW_RETURN) || is_simple(KW_BREAK) ||
		is_simple(KW_CONTINUE) || is_simple(KW_GOTO) || is_simple(KW_THROW))
		return parse_jump_statement();
	if (is_simple(KW_CASE) || is_simple(KW_DEFAULT))
		return parse_labeled_statement();
	if (is_kind(PA6_IDENTIFIER_TOKEN) && is_simple(OP_COLON, pos_ + 1))
		return parse_labeled_statement();

	// A known value at statement start cannot be reinterpreted as a type.  For
	// unknown names, declaration-first is the C++ ambiguity rule and the
	// parser still falls back transactionally when the declaration is invalid.
	if (can_start_declaration())
	{
		const BindKind* binding = is_kind(PA6_IDENTIFIER_TOKEN) ?
			scopes_.Lookup(token(pos_).spelling) : 0;
		// A qualified type followed by a parenthesized pointer declarator is
		// a declaration even though the first component is a namespace.  The
		// syntactic scope table intentionally does not duplicate namespace
		// members, so recognize this unambiguous declarator shape before the
		// declaration-vs-call fallback below.
		const bool qualified_pointer_declaration =
			binding != 0 && *binding == BIND_NAMESPACE &&
			is_simple(OP_COLON2, pos_ + 1) &&
			is_kind(PA6_IDENTIFIER_TOKEN, pos_ + 2) &&
			is_simple(OP_LPAREN, pos_ + 3) &&
			(is_simple(OP_STAR, pos_ + 4) || is_simple(OP_AMP, pos_ + 4) ||
			 is_simple(OP_LAND, pos_ + 4)) &&
			is_kind(PA6_IDENTIFIER_TOKEN, pos_ + 5) &&
			is_simple(OP_RPAREN, pos_ + 6) &&
			is_simple(OP_LPAREN, pos_ + 7);
		// A qualified class name followed by a call is declaration-shaped in
		// the grammar, but it is an expression when the final component names
		// a static member function.  Keep the tentative expression available
		// for semantic lookup; a real type-name declaration still falls back
		// transactionally below when this parse cannot form a call.
		const bool qualified_type_call =
			binding != 0 &&
			(*binding == BIND_TYPE || *binding == BIND_TEMPLATE) &&
			is_simple(OP_COLON2, pos_ + 1) &&
			is_kind(PA6_IDENTIFIER_TOKEN, pos_ + 2) &&
			is_simple(OP_LPAREN, pos_ + 3);
		if (is_kind(PA6_IDENTIFIER_TOKEN) &&
			(binding == 0 || *binding == BIND_NAMESPACE || qualified_type_call) &&
			!qualified_pointer_declaration)
		{
			const Mark expression_mark = mark();
			AstId expression = parse_expression();
			if (expression != 0 && node_has_kind(expression, AST_CALL_EXPRESSION) &&
				consume_simple(OP_SEMICOLON))
			{
				const AstId result = make(AST_EXPRESSION_STATEMENT);
				add(result, expression);
				return result;
			}
			restore(expression_mark);
		}
		if (binding == 0 || *binding != BIND_VALUE ||
			is_simple(OP_COLON2, pos_ + 1))
		{
			const Mark saved = mark();
			AstId declaration = parse_declaration();
			if (declaration != 0)
				return declaration;
			restore(saved);
		}
	}
	return parse_expression_statement();
}

AstId Pa10Parser::parse_expression_statement()
{
	const Mark saved = mark();
	if (consume_simple(OP_SEMICOLON))
		return make(AST_EXPRESSION_STATEMENT);
	AstId expression = parse_expression();
	if (expression == 0 || !consume_simple(OP_SEMICOLON))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_EXPRESSION_STATEMENT);
	add(result, expression);
	return result;
}

AstId Pa10Parser::parse_compound_statement()
{
	const Mark saved = mark();
	if (!enter_bracket(OP_LBRACE))
		return 0;
	scopes_.Push();
	const AstId result = make(AST_COMPOUND_STATEMENT);
	while (!is_simple(OP_RBRACE))
	{
		if (at_end())
		{
			scopes_.Pop();
			restore(saved);
			return 0;
		}
		AstId statement = parse_statement();
		if (statement == 0)
		{
			scopes_.Pop();
			restore(saved);
			return 0;
		}
		add(result, statement);
	}
	if (!leave_bracket(OP_RBRACE))
	{
		scopes_.Pop();
		restore(saved);
		return 0;
	}
	scopes_.Pop();
	return result;
}

AstId Pa10Parser::parse_selection_statement()
{
	const Mark saved = mark();
	if (is_simple(KW_IF))
	{
		++pos_;
		AstId condition = parse_condition();
		AstId then_statement = condition == 0 ? 0 : parse_statement();
		if (condition == 0 || then_statement == 0)
		{
			restore(saved);
			return 0;
		}
		const AstId result = make(AST_IF_STATEMENT);
		add(result, condition);
		const AstId then_node = make(AST_THEN);
		add(then_node, then_statement);
		add(result, then_node);
		if (consume_simple(KW_ELSE))
		{
			AstId else_statement = parse_statement();
			if (else_statement == 0)
			{
				restore(saved);
				return 0;
			}
			const AstId else_node = make(AST_ELSE);
			add(else_node, else_statement);
			add(result, else_node);
		}
		return result;
	}
	if (is_simple(KW_WHILE))
	{
		++pos_;
		AstId condition = parse_condition();
		AstId statement = condition == 0 ? 0 : parse_statement();
		if (condition == 0 || statement == 0)
		{
			restore(saved);
			return 0;
		}
		const AstId result = make(AST_WHILE_STATEMENT);
		add(result, condition);
		add(result, statement);
		return result;
	}
	if (is_simple(KW_SWITCH))
	{
		++pos_;
		AstId condition = parse_condition();
		AstId statement = condition == 0 ? 0 : parse_statement();
		if (condition == 0 || statement == 0)
		{
			restore(saved);
			return 0;
		}
		const AstId result = make(AST_SWITCH_STATEMENT);
		add(result, condition);
		add(result, statement);
		return result;
	}
	if (is_simple(KW_DO))
	{
		++pos_;
		AstId statement = parse_statement();
		if (statement == 0 || !consume_simple(KW_WHILE))
		{
			restore(saved);
			return 0;
		}
		AstId condition = parse_condition();
		if (condition == 0 || !consume_simple(OP_SEMICOLON))
		{
			restore(saved);
			return 0;
		}
		const AstId result = make(AST_DO_STATEMENT);
		add(result, statement);
		add(result, condition);
		return result;
	}
	if (is_simple(KW_FOR))
		return parse_iteration_statement();
	restore(saved);
	return 0;
}

AstId Pa10Parser::parse_condition()
{
	const Mark saved = mark();
	if (!enter_bracket(OP_LPAREN))
		return 0;
	const Mark inside = mark();
	AstId specifiers = parse_decl_specifier_seq();
	AstId declarator = specifiers == 0 ? 0 : parse_declarator();
	AstId initializer = declarator == 0 ? 0 : parse_initializer();
	if (specifiers != 0 && declarator != 0 && initializer != 0 &&
		leave_bracket(OP_RPAREN))
	{
		const AstId declaration = make(AST_CONDITION_DECLARATION);
		add(declaration, specifiers);
		add(declaration, declarator);
		add(declaration, initializer);
		const AstId result = make(AST_CONDITION);
		add(result, declaration);
		return result;
	}
	restore(inside);
	AstId expression = parse_expression();
	if (expression == 0 || !leave_bracket(OP_RPAREN))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_CONDITION);
	add(result, expression);
	return result;
}

AstId Pa10Parser::parse_iteration_statement()
{
	const Mark saved = mark();
	if (!consume_simple(KW_FOR) || !enter_bracket(OP_LPAREN))
		return 0;
	AstId init = parse_for_init_statement();
	if (init == 0)
	{
		restore(saved);
		return 0;
	}
	AstId condition = 0;
	if (!is_simple(OP_SEMICOLON))
		condition = parse_expression();
	if ((!is_simple(OP_SEMICOLON)) || !consume_simple(OP_SEMICOLON))
	{
		restore(saved);
		return 0;
	}
	AstId iteration = 0;
	if (!is_simple(OP_RPAREN))
		iteration = parse_expression();
	if ((!is_simple(OP_RPAREN)) || !leave_bracket(OP_RPAREN))
	{
		restore(saved);
		return 0;
	}
	AstId statement = parse_statement();
	if (statement == 0)
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_FOR_STATEMENT);
	add(result, init);
	if (condition != 0)
	{
		const AstId condition_node = make(AST_CONDITION);
		add(condition_node, condition);
		add(result, condition_node);
	}
	if (iteration != 0)
	{
		const AstId iteration_node = make(AST_ITERATION);
		add(iteration_node, iteration);
		add(result, iteration_node);
	}
	add(result, statement);
	return result;
}

AstId Pa10Parser::parse_for_init_statement()
{
	const Mark saved = mark();
	if (is_simple(OP_SEMICOLON))
	{
		++pos_;
		return make(AST_FOR_INIT_STATEMENT);
	}
	AstId declaration = parse_specified_declaration(false);
	if (declaration != 0)
	{
		const AstId result = make(AST_FOR_INIT_STATEMENT);
		add(result, declaration);
		return result;
	}
	restore(saved);
	AstId expression = parse_expression();
	if (expression == 0 || !consume_simple(OP_SEMICOLON))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_FOR_INIT_STATEMENT);
	add(result, expression);
	return result;
}

AstId Pa10Parser::parse_labeled_statement()
{
	const Mark saved = mark();
	if (consume_simple(KW_CASE))
	{
		AstId expression = parse_expression();
		if (expression == 0 || !consume_simple(OP_COLON))
		{
			restore(saved);
			return 0;
		}
		AstId statement = parse_statement();
		if (statement == 0)
		{
			restore(saved);
			return 0;
		}
		const AstId result = make(AST_CASE_STATEMENT);
		add(result, expression);
		add(result, statement);
		return result;
	}
	if (consume_simple(KW_DEFAULT))
	{
		if (!consume_simple(OP_COLON))
		{
			restore(saved);
			return 0;
		}
		AstId statement = parse_statement();
		if (statement == 0)
		{
			restore(saved);
			return 0;
		}
		const AstId result = make(AST_DEFAULT_STATEMENT);
		add(result, statement);
		return result;
	}
	if (!is_kind(PA6_IDENTIFIER_TOKEN) || !is_simple(OP_COLON, pos_ + 1))
		return 0;
	const size_t name_at = pos_;
	pos_ += 2;
	AstId statement = parse_statement();
	if (statement == 0)
	{
		restore(saved);
		return 0;
	}
	const AstId result = make_span(AST_LABELED_STATEMENT, name_at, name_at + 1,
		token(name_at).spelling);
	add(result, statement);
	return result;
}

AstId Pa10Parser::parse_jump_statement()
{
	const Mark saved = mark();
	if (consume_simple(KW_BREAK))
	{
		if (!consume_simple(OP_SEMICOLON))
		{
			restore(saved);
			return 0;
		}
		return make(AST_BREAK_STATEMENT);
	}
	if (consume_simple(KW_CONTINUE))
	{
		if (!consume_simple(OP_SEMICOLON))
		{
			restore(saved);
			return 0;
		}
		return make(AST_CONTINUE_STATEMENT);
	}
	if (consume_simple(KW_GOTO))
	{
		if (!is_kind(PA6_IDENTIFIER_TOKEN))
		{
			restore(saved);
			return 0;
		}
		const size_t name_at = pos_++;
		if (!consume_simple(OP_SEMICOLON))
		{
			restore(saved);
			return 0;
		}
		return make_span(AST_GOTO_STATEMENT, name_at, name_at + 1,
			token(name_at).spelling);
	}
	if (consume_simple(KW_RETURN))
	{
		AstId expression = 0;
		if (!is_simple(OP_SEMICOLON))
			expression = is_simple(OP_LBRACE) ? parse_braced_init_list() :
			parse_expression();
		if (!is_simple(OP_SEMICOLON) || !consume_simple(OP_SEMICOLON))
		{
			restore(saved);
			return 0;
		}
		const AstId result = make(AST_RETURN_STATEMENT);
		add(result, expression);
		return result;
	}
	if (consume_simple(KW_THROW))
	{
		AstId expression = 0;
		if (!is_simple(OP_SEMICOLON))
			expression = parse_expression();
		if (!is_simple(OP_SEMICOLON) || !consume_simple(OP_SEMICOLON))
		{
			restore(saved);
			return 0;
		}
		const AstId result = make(AST_THROW_STATEMENT);
		add(result, expression);
		return result;
	}
	restore(saved);
	return 0;
}

AstId Pa10Parser::parse_try_block()
{
	const Mark saved = mark();
	if (!consume_simple(KW_TRY))
		return 0;
	AstId compound = parse_compound_statement();
	if (compound == 0)
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_TRY_BLOCK);
	add(result, compound);
	AstId handler = 0;
	while (is_simple(KW_CATCH))
	{
		handler = parse_handler();
		if (handler == 0)
		{
			restore(saved);
			return 0;
		}
		add(result, handler);
	}
	return handler == 0 ? (restore(saved), static_cast<AstId>(0)) : result;
}

AstId Pa10Parser::parse_handler()
{
	const Mark saved = mark();
	if (!consume_simple(KW_CATCH) || !enter_bracket(OP_LPAREN))
		return 0;
	AstId declaration = parse_exception_declaration();
	if (declaration == 0 || !leave_bracket(OP_RPAREN))
	{
		restore(saved);
		return 0;
	}
	AstId body = parse_compound_statement();
	if (body == 0)
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_HANDLER);
	add(result, declaration);
	add(result, body);
	return result;
}

AstId Pa10Parser::parse_exception_declaration()
{
	const Mark saved = mark();
	if (is_simple(OP_DOTS))
	{
		const AstId result = make(AST_EXCEPTION_DECLARATION);
		add(result, make_span(AST_ELLIPSIS, pos_, pos_ + 1, "..."));
		++pos_;
		return result;
	}
	AstId specifiers = parse_decl_specifier_seq();
	if (specifiers == 0)
	{
		restore(saved);
		return 0;
	}
	AstId declarator = 0;
	if (!is_simple(OP_RPAREN))
		declarator = parse_declarator(true);
	if (!is_simple(OP_RPAREN))
	{
		restore(saved);
		return 0;
	}
	const AstId result = make(AST_EXCEPTION_DECLARATION);
	add(result, specifiers);
	add(result, declarator);
	return result;
}
