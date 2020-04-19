/*********************                                                        */
/*! \file
 ** \verbatim
 ** Top contributors (to current version):
 **   Makai Mann, Ahmed Irfan
 ** This file is part of the cosa2 project.
 ** Copyright (c) 2019 by the authors listed in the file AUTHORS
 ** in the top-level source directory) and their institutional affiliations.
 ** All rights reserved.  See the file LICENSE in the top-level source
 ** directory for licensing information.\endverbatim
 **
 ** \brief
 **
 **
 **/

#include <iostream>
#include "assert.h"
#include "math.h"

#include "optionparser.h"
#include "smt-switch/boolector_factory.h"
#ifdef WITH_MSAT
  #include "smt-switch/msat_factory.h"
#endif

#include "bmc.h"
#include "bmc_simplepath.h"
#include "core/fts.h"
#include "defaults.h"
#include "frontends/btor2_encoder.h"
#include "frontends/smv_encoder.h"
#include "interpolant.h"
#include "kinduction.h"
#include "printers/btor2_witness_printer.h"
#include "printers/vcd_witness_printer.h"
#include "prop.h"
#include "utils/logger.h"

using namespace cosa;
using namespace smt;
using namespace std;

/************************************* Option Handling setup
 * *****************************************/
// from optionparser-1.7 examples -- example_arg.cc
enum optionIndex
{
  UNKNOWN_OPTION,
  HELP,
  ENGINE,
  BOUND,
  PROP,
  VERBOSITY,
  VCDNAME,
  RESET,
  RESET_BND,
  CLK
};

struct Arg : public option::Arg
{
  static void printError(const char * msg1,
                         const option::Option & opt,
                         const char * msg2)
  {
    fprintf(stderr, "%s", msg1);
    fwrite(opt.name, opt.namelen, 1, stderr);
    fprintf(stderr, "%s", msg2);
  }

  static option::ArgStatus Numeric(const option::Option & option, bool msg)
  {
    char * endptr = 0;
    if (option.arg != 0 && strtol(option.arg, &endptr, 10)) {
    };
    if (endptr != option.arg && *endptr == 0) return option::ARG_OK;

    if (msg) printError("Option '", option, "' requires a numeric argument\n");
    return option::ARG_ILLEGAL;
  }

  static option::ArgStatus NonEmpty(const option::Option & option, bool msg)
  {
    if (option.arg != 0 && option.arg[0] != 0) return option::ARG_OK;

    if (msg)
      printError("Option '", option, "' requires a non-empty argument\n");
    return option::ARG_ILLEGAL;
  }
};

const option::Descriptor usage[] = {
  { UNKNOWN_OPTION,
    0,
    "",
    "",
    Arg::None,
    "USAGE: cosa2 [options] <btor file>\n\n"
    "Options:" },
  { HELP, 0, "", "help", Arg::None, "  --help \tPrint usage and exit." },
  { ENGINE,
    0,
    "e",
    "engine",
    Arg::NonEmpty,
    "  --engine, -e <engine> \tSelect engine from [bmc, bmc-sp, ind, "
    "interp]." },
  { BOUND,
    0,
    "k",
    "bound",
    Arg::Numeric,
    "  --bound, -k \tBound to check up until (default: 10)." },
  { PROP,
    0,
    "p",
    "prop",
    Arg::Numeric,
    "  --prop, -p \tProperty index to check (default: 0)." },
  { VERBOSITY,
    0,
    "v",
    "verbosity",
    Arg::Numeric,
    "  --verbosity, -v \tVerbosity for printing to standard out." },
  { VCDNAME,
    0,
    "",
    "vcd",
    Arg::NonEmpty,
    "  --vcd \tName of Value Change Dump (VCD) if witness exists." },
  { RESET,
    0,
    "r",
    "reset",
    Arg::NonEmpty,
    " --reset, -r <reset input> \tInput to use for reset signal (prefix with ~ "
    "for negative reset)" },
  { RESET_BND,
    0,
    "s",
    "resetsteps",
    Arg::Numeric,
    " --resetsteps, -s <integer> \tNumber of steps to apply reset for "
    "(default: 1)" },
  { CLK,
    0,
    "c",
    "clock",
    Arg::NonEmpty,
    " --clock, -c <clock name> \tInput to use for clock signal (only supports "
    "starting at 0 and toggling each step)" },
  { 0, 0, 0, 0, 0, 0 }
};
/*********************************** end Option Handling setup
 * ***************************************/

ProverResult check_prop(Engine engine,
                        unsigned int bound,
                        Property & p,
                        SmtSolver & s,
                        SmtSolver & second_solver,
                        std::vector<UnorderedTermMap> & cex)
{
  logger.log(1, "Solving property: {}", p.prop());

  logger.log(3, "INIT:\n{}", p.transition_system().init());
  logger.log(3, "TRANS:\n{}", p.transition_system().trans());

  std::shared_ptr<Prover> prover;
  if (engine == BMC) {
    prover = std::make_shared<Bmc>(p, s);
  } else if (engine == BMC_SP) {
    prover = std::make_shared<BmcSimplePath>(p, s);
  } else if (engine == KIND) {
    prover = std::make_shared<KInduction>(p, s);
  } else if (engine == INTERP) {
    assert(second_solver != NULL);
    prover = std::make_shared<InterpolantMC>(p, s, second_solver);
  } else {
    throw CosaException("Unimplemented engine.");
  }

  ProverResult r = prover->check_until(bound);
  if (r == FALSE) {
    prover->witness(cex);
  }
  return r;
}

smt::Term add_reset_and_clock(std::string reset_name,
                              std::string clock_name,
                              smt::Term prop_term,
                              smt::TransitionSystem & ts,
                              smt::SmtSolver & s)
{
  // add reset logic if requested
  if (!reset_name.empty()) {
    bool negative_reset = false;

    if (reset_name.at(0) == '~') {
      reset_name = reset_name.substr(1, reset_name.length() - 1);
      negative_reset = true;
    }

    Term reset_symbol;
    bool found_reset_symbol = false;
    for (auto i : rts.inputs()) {
      if (i->to_string() == reset_name) {
        reset_symbol = i;
        found_reset_symbol = true;
        break;
      }
    }

    if (!found_reset_symbol) {
      logger.log(0, "Could not find reset symbol: {}", reset_name);
      return 3;
    }

    if (reset_symbol->get_sort()->get_sort_kind() != BV
        || reset_symbol->get_sort()->get_width() != 1) {
      logger.log(
          0, "Unexpected reset symbol sort: {}", reset_symbol->get_sort());
      return 3;
    }

    Sort one_bit_sort = s->make_sort(BV, 1);
    uint32_t num_bits = ceil(log2(reset_bnd)) + 1;
    Sort bvsort = s->make_sort(BV, num_bits);
    Term reset_bnd_term = s->make_term(reset_bnd, bvsort);
    Term reset_counter = rts.make_state("__internal_cosa2_reset_cnt__", bvsort);

    Term in_reset = s->make_term(BVUlt, reset_counter, reset_bnd_term);
    Term reset_done = s->make_term(Not, in_reset);

    rts.constrain_init(
        s->make_term(Equal, reset_counter, s->make_term(0, bvsort)));
    rts.set_next(
        reset_counter,
        s->make_term(
            Ite,
            reset_done,
            reset_counter,
            s->make_term(BVAdd, reset_counter, s->make_term(1, bvsort))));

    Term active_reset =
        negative_reset
            ? s->make_term(Equal, reset_symbol, s->make_term(0, one_bit_sort))
            : s->make_term(Equal, reset_symbol, s->make_term(1, one_bit_sort));
    Term inactive_reset = s->make_term(Not, active_reset);
    rts.add_invar(s->make_term(Implies, in_reset, active_reset));
    rts.add_invar(s->make_term(Implies, reset_done, inactive_reset));

    prop_term = s->make_term(Implies, reset_done, prop_term);
  }

  // toggle clock if requested (only supporting toggling every step and starting
  // at 0 for now)
  if (!clock_name.empty()) {
    Term clock_symbol;
    bool found_clock_symbol = false;
    for (auto i : rts.inputs()) {
      if (i->to_string() == clock_name) {
        clock_symbol = i;
        found_clock_symbol = true;
        break;
      }
    }

    if (!found_clock_symbol) {
      logger.log(0, "Could not find clock symbol: {}", clock_name);
      return 3;
    }

    Sort one_bit_sort = s->make_sort(BV, 1);

    if (clock_symbol->get_sort() != one_bit_sort) {
      throw CosaException("Expecting a one-bit clock sort.");
    }

    Term zero = s->make_term(0, one_bit_sort);
    rts.constrain_init(s->make_term(Equal, clock_symbol, zero));
    rts.constrain_trans(s->make_term(
        Equal, rts.next(clock_symbol), s->make_term(BVNot, clock_symbol)));
  }

  return prop_term;
}

int main(int argc, char ** argv)
{
  argc -= (argc > 0);
  argv += (argc > 0);  // skip program name argv[0] if present
  option::Stats stats(usage, argc, argv);
  std::vector<option::Option> options(stats.options_max);
  std::vector<option::Option> buffer(stats.buffer_max);
  option::Parser parse(usage, argc, argv, &options[0], &buffer[0]);

  if (parse.error()) {
    return 3;
  }

  if (options[HELP] || argc == 0) {
    option::printUsage(cout, usage);
    return 2;  // unknown is 2
  }

  if (parse.nonOptionsCount() != 1) {
    option::printUsage(cout, usage);
    return 3;
  }

  bool unknown_options = false;
  for (option::Option * opt = options[UNKNOWN_OPTION]; opt; opt = opt->next()) {
    unknown_options = true;
  }

  if (unknown_options) {
    option::printUsage(cout, usage);
    return 3;
  }

  Engine engine = default_engine;
  unsigned int prop_idx = default_prop_idx;
  unsigned int bound = default_bound;
  unsigned int verbosity = default_verbosity;
  std::string vcd_name;
  std::string reset_name = default_reset_name;
  unsigned int reset_bnd = default_reset_bnd;
  std::string clock_name = default_clock_name;

  for (int i = 0; i < parse.optionsCount(); ++i) {
    option::Option & opt = buffer[i];
    switch (opt.index()) {
      case HELP:
        // not possible, because handled further above and exits the program
      case ENGINE: engine = to_engine(opt.arg); break;
      case BOUND: bound = atoi(opt.arg); break;
      case PROP: prop_idx = atoi(opt.arg); break;
      case VERBOSITY: verbosity = atoi(opt.arg); break;
      case VCDNAME: vcd_name = opt.arg; break;
      case RESET: reset_name = opt.arg; break;
      case RESET_BND: reset_bnd = atoi(opt.arg); break;
      case CLK: clock_name = opt.arg; break;
      case UNKNOWN_OPTION:
        // not possible because Arg::Unknown returns ARG_ILLEGAL
        // which aborts the parse with an error
        break;
    }
  }

  // set logger verbosity -- can only be set once
  logger.set_verbosity(verbosity);

  string filename(parse.nonOption(0));

  int status_code = 3;

  try {
    SmtSolver s;
    SmtSolver second_solver;
    if (engine == INTERP) {
      #ifdef WITH_MSAT
      // need mathsat for interpolant based model checking
      s = MsatSolverFactory::create();
      second_solver = MsatSolverFactory::create_interpolating_solver();
      #else
      throw CosaException("Interpolation-based model checking requires MathSAT and "
                          "this version of cosa2 is built without MathSAT.\nPlease "
                          "setup smt-switch with MathSAT and reconfigure using --with-msat.\n"
                          "Note: MathSAT has a custom license and you must assume all "
                          "responsibility for meeting the license requirements.");
      #endif
    } else {
      // boolector is faster but doesn't support interpolants
      s = BoolectorSolverFactory::create();
      s->set_opt("produce-models", "true");
      s->set_opt("incremental", "true");
    }

    // TODO: make this less ugly, just need to keep it in scope if using
    //       it would be better to have a generic encoder
    //       and also only create the transition system once
    ProverResult r;
    string file_ext = filename.substr(filename.find_last_of(".") + 1);
    if (file_ext == "btor2" || file_ext == "btor") {
      logger.log(2, "Parsing BTOR2 file: {}", filename);
      FunctionalTransitionSystem fts(s);
      BTOR2Encoder btor_enc(filename, fts);
      const TermVec & propvec = btor_enc.propvec();
      unsigned int num_props = propvec.size();
      if (prop_idx >= num_props) {
        throw CosaException(
            "Property index " + to_string(prop_idx)
            + " is greater than the number of properties in file " + filename
            + " (" + to_string(num_props) + ")");
      }
      Term prop = propvec[prop_idx];
      prop = add_reset_and_clock(reset_name, clock_name, prop, fts, s);
      Property p(fts, prop);
      vector<UnorderedTermMap> cex;

      r = check_prop(engine, bound, p, s, second_solver, cex);

      // print btor output
      if (r == FALSE) {
        cout << "sat" << endl;
        cout << "b" << prop_idx << endl;
        if (cex.size()) {
          print_witness_btor(btor_enc, cex);
          if (!vcd_name.empty()) {
            VCDWitnessPrinter vcdprinter(fts, cex);
            vcdprinter.DumpTraceToFile(vcd_name);
          }
        }
        status_code = 1;
      } else if (r == TRUE) {
        cout << "unsat" << endl;
        cout << "b" << prop_idx << endl;
        status_code = 0;
      } else {
        cout << "unknown" << endl;
        cout << "b" << prop_idx << endl;
        status_code = 2;
      }

    } else if (file_ext == "smv") {
      logger.log(2, "Parsing SMV file: {}", filename);
      RelationalTransitionSystem rts(s);
      SMVEncoder smv_enc(filename, rts);
      const TermVec & propvec = smv_enc.propvec();
      unsigned int num_props = propvec.size();
      if (prop_idx >= num_props) {
        throw CosaException(
            "Property index " + to_string(prop_idx)
            + " is greater than the number of properties in file " + filename
            + " (" + to_string(num_props) + ")");
      }
      Term prop = propvec[prop_idx];
      prop = add_reset_and_clock(reset_name, clock_name, prop, fts, s);
      Property p(rts, prop);
      std::vector<UnorderedTermMap> cex;

      r = check_prop(engine, bound, p, s, second_solver, cex);
      logger.log(0, "Property {} is {}", prop_idx, to_string(r));

      if (r == FALSE) {
        for (size_t t = 0; t < cex.size(); t++) {
          cout << "AT TIME " << t << endl;
          for (auto elem : cex[t]) {
            cout << "\t" << elem.first << " : " << elem.second << endl;
          }
        }
        if (!vcd_name.empty()) {
          VCDWitnessPrinter vcdprinter(rts, cex);
          vcdprinter.DumpTraceToFile(vcd_name);
        }
      }
    } else {
      throw CosaException("Unrecognized file extension " + file_ext
                          + " for file " + filename);
    }
  }
  catch (CosaException & ce) {
    cout << ce.what() << endl;
    cout << "unknown" << endl;
    cout << "b" << prop_idx << endl;
  }
  catch (SmtException & se) {
    cout << se.what() << endl;
    cout << "unknown" << endl;
    cout << "b" << prop_idx << endl;
  }
  catch (std::exception & e) {
    cout << "Caught generic exception..." << endl;
    cout << e.what() << endl;
    cout << "unknown" << endl;
    cout << "b" << prop_idx << endl;
  }

  return status_code;
}
