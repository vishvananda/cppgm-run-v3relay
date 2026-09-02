// Student-facing scaffold for the PA13 `lowir2cy86` binary.

#include "exceptions.h"
#include "lowir_cy86_codegen.h"
#include "lowir_model.h"
#include "lowir_validate.h"
#include "tool_help_text.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

namespace {

vector<string> collect_args(int argc, char ** argv)
{
  vector<string> args;
  for(int i = 1; i < argc; ++i) {
    args.push_back(argv[i]);
  }
  return args;
}

bool has_help_arg(const vector<string> & args)
{
  for(size_t i = 0; i < args.size(); ++i) {
    if(args[i] == "--help" || args[i] == "-h") {
      return true;
    }
  }
  return false;
}

bool has_batch_stdin_arg(const vector<string> & args)
{
  for(size_t i = 0; i < args.size(); ++i) {
    if(args[i] == "--batch-stdin") {
      return true;
    }
  }
  return false;
}

int run_not_implemented_batch_mode()
{
  string line;
  while(getline(cin, line)) {
    (void)line;
    cout << "EXIT_NOT_IMPLEMENTED" << endl;
  }
  return EXIT_SUCCESS;
}

void parse_output_invocation(const vector<string> & args,
                             string & outfile,
                             vector<string> & srcfiles)
{
  if(args.size() < 3 || args[0] != "-o") {
    throw logic_error("invalid usage");
  }

  outfile = args[1];
  srcfiles.assign(args.begin() + 2, args.end());
}

int run_lowir2cy86_mode(const vector<string> & args)
{
  if(has_batch_stdin_arg(args)) {
    return run_not_implemented_batch_mode();
  }

  if(has_help_arg(args)) {
    cout << lowir2cy86_help_text();
    return EXIT_SUCCESS;
  }

  string outfile;
  vector<string> srcfiles;
  parse_output_invocation(args, outfile, srcfiles);

  const lowir_model::LowirProgram program =
    lowir_model::parse_lowir_program_files(srcfiles);
  const LowirProgramFacts facts = ValidateLowirProgram(program);
  const string output = EmitCy86Program(program, facts);
  ofstream destination(outfile.c_str(), ios::out | ios::binary | ios::trunc);
  if(!destination) {
    throw runtime_error("unable to open output file '" + outfile + "'");
  }
  destination << output;
  if(!destination) {
    throw runtime_error("unable to write output file '" + outfile + "'");
  }
  return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char ** argv)
{
  try
  {
    return run_lowir2cy86_mode(collect_args(argc, argv));
  }
  catch(const NotImplementedException & e)
  {
    cerr << "ERROR: " << e.what() << endl;
    return CPPGM_EXIT_NOT_IMPLEMENTED;
  }
  catch(const exception & e)
  {
    cerr << "ERROR: " << e.what() << endl;
    return EXIT_FAILURE;
  }
}
