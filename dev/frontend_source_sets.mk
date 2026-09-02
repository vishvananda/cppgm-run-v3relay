# Per-tool implementation source lists for the compiler.
#
# Add dev/src/foo.cpp to the tools that use it by adding `foo` below. For
# subdirectories, use the path without `.cpp`, such as `parser/foo`.

FRONTEND_SOURCE_SET_TARGETS := abimangle pptoken posttoken ctrlexpr macro preproc recog nsdecl nsinit cy86 cppgm++ lowiropt lowir2cy86 lowir2native

FRONTEND_OBJ_BASENAMES_abimangle :=
FRONTEND_OBJ_BASENAMES_pptoken := pptoken_lexer unicode
FRONTEND_OBJ_BASENAMES_posttoken := pptoken_lexer posttoken_stream posttoken_tables unicode
FRONTEND_OBJ_BASENAMES_ctrlexpr := ctrlexpr_eval pptoken_lexer posttoken_stream posttoken_tables unicode
FRONTEND_OBJ_BASENAMES_macro := macro_replace pptoken_lexer posttoken_stream posttoken_tables unicode
FRONTEND_OBJ_BASENAMES_preproc := preproc_host preproc_engine macro_replace ctrlexpr_eval pptoken_lexer posttoken_stream posttoken_tables unicode
FRONTEND_OBJ_BASENAMES_recog := preproc_host parser/recog_parser parser/recog_parser_cp2 parser/recog_token \
	preproc_engine macro_replace ctrlexpr_eval pptoken_lexer \
	posttoken_stream posttoken_tables unicode
FRONTEND_OBJ_BASENAMES_nsdecl := preproc_host parser/nsdecl_parser parser/nsdecl_model parser/recog_token \
	preproc_engine macro_replace ctrlexpr_eval pptoken_lexer \
	posttoken_stream posttoken_tables unicode
FRONTEND_OBJ_BASENAMES_nsinit := preproc_host parser/nsdecl_parser parser/nsdecl_model \
	parser/nsinit_sema parser/nsinit_image parser/recog_token \
	preproc_engine macro_replace ctrlexpr_eval pptoken_lexer \
	posttoken_stream posttoken_tables unicode
FRONTEND_OBJ_BASENAMES_cy86 := preproc_host cy86_parse cy86_codegen x86_assembler \
	preproc_engine macro_replace ctrlexpr_eval pptoken_lexer \
	posttoken_stream posttoken_tables unicode
FRONTEND_OBJ_BASENAMES_cppgm++ := preproc_host parser/ast_parser parser/ast_parser_decl \
	parser/ast_parser_expr parser/ast_scope parser/ast_model parser/recog_token \
	sema/type_table sema/qualified_name sema/scope_model sema/const_eval \
	sema/sema_tree sema/conversions sema/overload sema/expr_sema sema/scope_builder \
	sema/type_builder sema/stmt_builder sema/semantics_dump sema/types_dump \
	preproc_engine macro_replace ctrlexpr_eval pptoken_lexer posttoken_stream \
	posttoken_tables unicode
FRONTEND_OBJ_BASENAMES_lowiropt :=
FRONTEND_OBJ_BASENAMES_lowir2cy86 :=
FRONTEND_OBJ_BASENAMES_lowir2native :=
