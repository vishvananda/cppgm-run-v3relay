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
FRONTEND_OBJ_BASENAMES_preproc := preproc_engine macro_replace ctrlexpr_eval pptoken_lexer posttoken_stream posttoken_tables unicode
FRONTEND_OBJ_BASENAMES_recog := parser/recog_parser parser/recog_parser_cp2 parser/recog_token \
	preproc_engine macro_replace ctrlexpr_eval pptoken_lexer \
	posttoken_stream posttoken_tables unicode
FRONTEND_OBJ_BASENAMES_nsdecl :=
FRONTEND_OBJ_BASENAMES_nsinit :=
FRONTEND_OBJ_BASENAMES_cy86 :=
FRONTEND_OBJ_BASENAMES_cppgm++ :=
FRONTEND_OBJ_BASENAMES_lowiropt :=
FRONTEND_OBJ_BASENAMES_lowir2cy86 :=
FRONTEND_OBJ_BASENAMES_lowir2native :=
