/** \copyright
 * Copyright (c) 2019, Balazs Racz
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are  permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * \file Driver.hxx
 *
 * Controls the parsing and compiling execution.
 *
 * @author Balazs Racz
 * @date 15 June 2019
 */

#ifndef _LOGIC_DRIVER_HXX_
#define _LOGIC_DRIVER_HXX_
#include <map>
#include <memory>
#include <string>

#include "logic/AST.hxx"
#include "logic/Parser.hxxout"

namespace logic {
logic::yy::Parser::symbol_type yylex(logic::Driver& driver);
}

// Declaration of the lexer function's prototype.
#define YY_DECL \
  logic::yy::Parser::symbol_type logic::yylex(logic::Driver& driver)
// This acutally invokes that declaration.
YY_DECL;

namespace logic {

// Encapsulation of the scanning and parsing process.
class Driver {
 public:
  /// Constructor.
  Driver() {}

  void clear() {
    global_context_.clear();
    function_context_.clear();
    current_context_ = &global_context_;
    commands_.clear();
    error_output_.clear();
    //next_guid_ = 1;
  }
  
  struct ParsingContext {
    /// Symbol table available in the current context.
    std::map<std::string, Symbol> symbol_table_;

    /// How many variables to allocate on the operand stack when entering this
    /// context.
    unsigned frame_size_{0};

    void clear() {
      frame_size_ = 0;
      symbol_table_.clear();
    }
  };

  ParsingContext global_context_;
  ParsingContext function_context_;
  ParsingContext* current_context_{&global_context_};
  
  /// @return the current syntactical context.
  ParsingContext* current_context() {
    return current_context_;
  }

  /// Called when starting to compile the body of a function.
  void enter_function() {
    current_context_ = &function_context_;
    function_context_.clear();
  }

  /// Called when leaving a function and going to the global scope.
  void exit_function() {
    current_context_ = &global_context_;
  }
  
  /// @return true if the currently parsing context is the global variable
  /// context.
  bool is_global_context() {
    return current_context_ == &global_context_;
  }
  
  /// Create a new local variable.
  /// @param name is the identifier ofthe local variable.
  /// @param type is the type (shall be LOCAL_VAR_NN).
  /// @return true if the allocation was successful.
  bool allocate_variable(const string& name, Symbol::Type type) {
    auto* s = allocate_symbol(name, type);
    if (!s) return false;
    s->fp_offset_ = current_context()->frame_size_;
    current_context()->frame_size_++;
    return true;
  }

  /// Create a new symbol.
  /// @param name is the name ofthe symbol. Will raise an error and return null
  /// if there is another symbol with the same name already.
  /// @param type is the symbol type.
  /// @return null upon error or the pointer to the newly created symbol table
  /// entry.
  Symbol* allocate_symbol(const string& name, Symbol::Type type) {
    if (current_context()->symbol_table_.find(name) !=
        current_context()->symbol_table_.end()) {
      string err = "Identifier '";
      err += name;
      err += "' is already declared.";
      error(err);
      return nullptr;
    }
    auto& s = current_context()->symbol_table_[name];
    s.symbol_type_ = type;
    return &s;
  }
  
  /// Looks up a symbol in the symbol table.
  /// @param name is the identifier of the symbol.
  /// @return symbol table entry, or null if this is an undeclared symbol.
  const Symbol* find_symbol(const string& name) {
    auto it = current_context()->symbol_table_.find(name);
    if (it == current_context()->symbol_table_.end()) {
      return nullptr;
    }
    return &it->second;
  }

  /// Looks up a symbol in the symbol table.
  /// @param name is the identifier of the symbol.
  /// @return symbol table entry, or null if this is an undeclared symbol.
  Symbol* find_mutable_symbol(const string& name) {
    auto it = current_context()->symbol_table_.find(name);
    if (it == current_context()->symbol_table_.end()) {
      return nullptr;
    }
    return &it->second;
  }

  
  /// Finds a variable type symbol in the symbol table. Reports an error and
  /// returns nullptr if the symbol is not found or not of the expected type.
  /// @param name is the symbol to look up.
  /// @param los is the location where the symbol was found; used for error printouts.
  /// @param expected_type describes what type of variable we are looking for.
  /// @return null upon error, or the symbol table entry.
  const Symbol* get_variable(const string& name, const yy::location& loc, Symbol::DataType expected_type) {
    const Symbol* s = find_symbol(name);
    if (!s) {
      string err = "Undeclared variable '";
      err += name;
      err += "'";
      error(loc, err);
      return nullptr;
    } else if (s->get_data_type() != expected_type) {
      string err = "'";
      err += name;
      err += "' incorrect type; expected ";
      err += Symbol::datatype_to_string(expected_type);
      err += ", actual type ";
      err += Symbol::datatype_to_string(s->get_data_type());
      error(loc, err);
      return nullptr;
    }
    return s;
  }

  std::unique_ptr<VariableReference> get_variable_reference(string name, const yy::location& loc, Symbol::DataType type) {
    const Symbol* s = get_variable(name, loc, type);
    if (!s) return nullptr;
    std::unique_ptr<VariableReference> r;
    switch(s->get_access()) {
      case Symbol::LOCAL_VAR:
        r.reset(new LocalVariableReference(std::move(name), s->fp_offset_));
        return r;
      case Symbol::INDIRECT_VAR:
        r.reset(new GlobalVariableReference(std::move(name), s->fp_offset_));
        return r;
      default:
        error(loc, "Unexpected storage modifier for variable.");
        return nullptr;
    }
  }
  
  /// The parsed AST of global statements.
  std::vector<std::shared_ptr<Command> > commands_;

  /// @return a debug printout of the entire AST after parsing.
  std::string debug_print() {
    std::string ret;
    for (const auto& c : commands_) {
      c->debug_print(&ret);
      ret.append(";\n");
    }
    return ret;
  }

  /// Serializes all commands into bytecode after the parsing.
  /// @param output it the container where the compiled bytecode will go.
  void serialize(std::string* output);

  /// lexical context variable that describes what storage option the current
  /// variable declaration has.
  VariableStorageSpecifier decl_storage_;

  /// True if we are in the global scope.
  // not currently used yet.
  //bool is_global_scope_;

  /// Accumulates error messages from the compiler.
  string error_output_;
  
  int result;
  // Run the parser on file F.
  // Return 0 on success.
  int parse_file(const std::string& filename);
  // Error handling.
  void error(const yy::location& l, const std::string& m);
  void error(const std::string& m);

  /// @todo make this const string*
  std::string* get_filename() {
    return &filename_;
  }

  /// Alocates a new GUID for an exported variable.
  int allocate_guid() { return next_guid_++; }

  /// @param bytecode is a pointer that was passed to serialize().
  /// @return true if this is the toplevel bytecode and thus we can trust that
  /// offsets are actual IPs.
  bool is_output_root(std::string* bytecode) {
    return (bytecode == output_root_);
  }
  
 private:
  /// The name of the file being parsed.
  /// Used later to pass the file name to the location tracker.
  std::string filename_;

  /// Caches the pointer to the string variable where we are serializing the
  /// compiled bytecode.
  std::string* output_root_;

  // Handling the scanner.
  void scan_begin();
  void scan_end();

  enum DebugLevel {
    NO_TRACE,
    TRACE_PARSE = 1,
    TRACE_LEX = 2,
    TRACE_LEX_AND_PARSE = 3,
  } debug_level_{NO_TRACE};

  /// Next GUID to assign to a variable.
  int next_guid_{1};

  /// The actual parser structures.
  yy::Parser parser{*this};
};

} // namespace logic

#endif  // _LOGIC_DRIVER_HXX_
