# Per-tool implementation source lists for the compiler.
#
# Add dev/src/foo.cpp to the tools that use it by adding `foo` below. For
# subdirectories, use the path without `.cpp`, such as `parser/foo`.

FRONTEND_SOURCE_SET_TARGETS := abimangle pptoken posttoken ppexpr preproc cppgm++ lowiropt lowir lowir2native
FRONTEND_TEST_RUNNER_SOURCE_ID := support/testing/test_runner

FRONTEND_OBJ_BASENAMES_abimangle :=
FRONTEND_OBJ_BASENAMES_pptoken := preprocess/tokens/PPTokenizer
FRONTEND_OBJ_BASENAMES_posttoken := preprocess/tokens/PPTokenizer
FRONTEND_OBJ_BASENAMES_ppexpr := preprocess/tokens/PPTokenizer preprocess/expressions/ControlExpression
FRONTEND_OBJ_BASENAMES_preproc := preprocess/tokens/PPTokenizer
FRONTEND_OBJ_BASENAMES_cppgm++ := preprocess/tokens/PPTokenizer
FRONTEND_OBJ_BASENAMES_lowiropt :=
FRONTEND_OBJ_BASENAMES_lowir :=
FRONTEND_OBJ_BASENAMES_lowir2native :=
