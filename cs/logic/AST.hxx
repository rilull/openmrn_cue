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
 * \file AST.hxx
 *
 * Abstract Syntax Tree for the logic language parser.
 *
 * @author Balazs Racz
 * @date 15 June 2019
 */

#ifndef _LOGIC_AST_HXX_
#define _LOGIC_AST_HXX_

#include <memory>

#include "utils/logging.h"
#include "utils/macros.h"
#include "utils/StringPrintf.hxx"

#include "logic/Bytecode.hxx"

namespace logic {

class Command {
 public:
  virtual ~Command() {}
  virtual void serialize(std::string* output) = 0;
  virtual void debug_print(std::string* output) = 0;
};

class EmptyCommand : public Command {
 public:
  void serialize(std::string* output) override {};
  void debug_print(std::string* output) override {};
};

class VariableReference {
 public:
  /// Create the VM commands to push the variable's value to the operand stack.
  virtual void serialize_fetch(std::string* output) = 0;

  /// Create the VM commands to take the value from the operand stack and store
  /// it into the variable.
  virtual void serialize_store(std::string* output) = 0;

  /// Print the variable representation to the sebug string.
  virtual void debug_print(std::string* output) = 0;
};

class LocalVariableReference : public VariableReference {
 public:
  LocalVariableReference(string name, int fp_offset)
      : name_(std::move(name)), fp_offset_(fp_offset) {}

  void serialize_fetch(std::string* output) override {
    BytecodeStream::append_opcode(output, LOAD_FP_REL);
    BytecodeStream::append_varint(output, fp_offset_);
  }

  /// Create the VM commands to take the value from the operand stack and store
  /// it into the variable.
  void serialize_store(std::string* output) override {
    BytecodeStream::append_opcode(output, STORE_FP_REL);
    BytecodeStream::append_varint(output, fp_offset_);
  }

  /// Print the variable representation to the sebug string.
  void debug_print(std::string* output) {
    output->append(name_);
  }

 private:
  std::string name_;
  /// FP-relative offset of the variable storage.
  int fp_offset_;
};

/// Compound command (aka brace enclosed command sequence).
class CommandSequence : public Command {
 public:
  CommandSequence(std::vector<std::shared_ptr<Command> > &&commands)
      : commands_(std::move(commands)) {}

  void serialize(std::string* output) override {
    for (const auto& c : commands_) {
      c->serialize(output);
    }
  }

  void debug_print(std::string* output) override {
    output->append("{\n");
    for (const auto& c : commands_) {
      c->debug_print(output);
      output->append("\n");
    }
    output->append("}\n");
  }

 private:
  std::vector<std::shared_ptr<Command> > commands_;
};

class IntExpression : public Command {};

class BooleanExpression : public Command {};


class NumericAssignment : public Command {
 public:
  /// Constructs a numeric assignment operation.
  /// @param variable is the string name of the variable (identifier; only for
  /// debugging)
  /// @param sym is the symbol describing what type this variable is and how to
  /// access it.
  /// @param value is the expression that computes the value to be stored
  /// (pushing exactly one entry to the operand stack).
  NumericAssignment(std::string variable, const Symbol& sym,
                    std::shared_ptr<IntExpression> value)
      : value_(std::move(value)) {
    if (sym.symbol_type_ == Symbol::LOCAL_VAR_INT) {
      variable_.reset(
          new LocalVariableReference(std::move(variable), sym.fp_offset_));
    } else {
      DIE("Unexpected symbol type");
    }
  }

  void serialize(std::string* output) override {
    value_->serialize(output);
    variable_->serialize_store(output);
  }

  void debug_print(std::string* output) override {
    output->append("assign(");
    variable_->debug_print(output);
    output->append(",");
    value_->debug_print(output);
    output->append(")");
  }

 private:
  std::unique_ptr<VariableReference> variable_;
  std::shared_ptr<IntExpression> value_;
};

class IntVariable : public IntExpression {
 public:
  IntVariable(std::string variable, const Symbol& sym) {
    if (sym.symbol_type_ == Symbol::LOCAL_VAR_INT) {
      variable_.reset(
          new LocalVariableReference(std::move(variable), sym.fp_offset_));
    } else {
      DIE("Unexpected symbol type");
    }
  }

  void serialize(std::string* output) override {
    variable_->serialize_fetch(output);
  }

  void debug_print(std::string* output) override {
    output->append("fetch(");
    variable_->debug_print(output);
    output->append(")");
  }

 private:
  std::unique_ptr<VariableReference> variable_;
};

class BooleanAssignment : public Command {
 public:
  /// Constructs a boolean assignment operation.
  /// @param variable is the string name of the variable (identifier; only for
  /// debugging)
  /// @param sym is the symbol describing what type this variable is and how to
  /// access it.
  /// @param value is the expression that computes the value to be stored
  /// (pushing exactly one entry to the operand stack).
  BooleanAssignment(std::string variable, Symbol sym,
                    std::shared_ptr<BooleanExpression> value)
      : variable_(std::move(variable)),
        sym_(std::move(sym)),
        value_(std::move(value)) {
    HASSERT(sym_.symbol_type_ == Symbol::LOCAL_VAR_BOOL);
  }

  void serialize(std::string* output) override {
    value_->serialize(output);
    BytecodeStream::append_opcode(output, STORE_FP_REL);
    BytecodeStream::append_varint(output, sym_.fp_offset_);
  }

  void debug_print(std::string* output) override {
    output->append("boolassign(");
    output->append(variable_);
    output->append(",");
    value_->debug_print(output);
    output->append(")");
  }

 private:
  std::string variable_;
  Symbol sym_;
  std::shared_ptr<BooleanExpression> value_;
};

class BoolVariable : public BooleanExpression {
 public:
  BoolVariable(std::string variable, const Symbol& sym) {
    if (sym.symbol_type_ == Symbol::LOCAL_VAR_BOOL) {
      variable_.reset(
          new LocalVariableReference(std::move(variable), sym.fp_offset_));
    } else {
      DIE("Unexpected symbol type");
    }
  }

  void serialize(std::string* output) override {
    variable_->serialize_fetch(output);
  }

  void debug_print(std::string* output) override {
    output->append("fetch(");
    variable_->debug_print(output);
    output->append(")");
  }

 private:
  std::unique_ptr<VariableReference> variable_;
};

/// Compound command (aka brace enclosed command sequence).
class IfThenElse : public Command {
 public:
  IfThenElse(std::shared_ptr<BooleanExpression> cond,
             std::shared_ptr<Command> then_case,
             std::shared_ptr<Command> else_case = nullptr)
      : condition_(std::move(cond)),
        then_case_(std::move(then_case)),
        else_case_(std::move(else_case)) {}

  void serialize(std::string* output) override {
    condition_->serialize(output);
    string then_block;
    then_case_->serialize(&then_block);
    string else_block;
    if (else_case_) {
      else_case_->serialize(&else_block);
      BytecodeStream::append_opcode(&then_block, JUMP);
      BytecodeStream::append_varint(&then_block, else_block.size());
    }
    BytecodeStream::append_opcode(output, TEST_JUMP_IF_FALSE);
    BytecodeStream::append_varint(output, then_block.size());
    output->append(then_block);
    output->append(else_block);
  }

  void debug_print(std::string* output) override {
    output->append("if (");
    condition_->debug_print(output);
    output->append(")");
    then_case_->debug_print(output);
    if (else_case_) {
      output->append(" else ");
      else_case_->debug_print(output);
    }
  }

 private:
  /// @todo this should rather be a boolean expression
  std::shared_ptr<BooleanExpression> condition_;
  std::shared_ptr<Command> then_case_;
  std::shared_ptr<Command> else_case_;
};

class IntBinaryExpression : public IntExpression {
 public:
  IntBinaryExpression(OpCode op, std::shared_ptr<IntExpression> left,
                      std::shared_ptr<IntExpression> right)
      : opcode_(op), left_(std::move(left)), right_(std::move(right)) {
    HASSERT(left_);
    HASSERT(right_);
  }

  void serialize(std::string* output) override {
    left_->serialize(output);
    right_->serialize(output);
    BytecodeStream::append_opcode(output, opcode_);
  }

  void debug_print(std::string* output) override {
    switch(opcode_) {
      case NUMERIC_PLUS:
        output->append("plus(");
        break;
      default:
        output->append("?\?\?(");
    }
    left_->debug_print(output);
    output->append(",");
    right_->debug_print(output);
    output->append(")");
  }

 private:
  /// Opcode that performs the given expression on the top of the stack.
  OpCode opcode_;
  std::shared_ptr<IntExpression> left_;
  std::shared_ptr<IntExpression> right_;
};

class IntConstant : public IntExpression {
 public:
  IntConstant(int value) : value_(value) {}

  void serialize(std::string* output) override {
    if (value_ == 0) {
      BytecodeStream::append_opcode(output, PUSH_CONSTANT_0);
    } else if (value_ == 1) {
      BytecodeStream::append_opcode(output, PUSH_CONSTANT_1);
    } else {
      BytecodeStream::append_opcode(output, PUSH_CONSTANT);
      BytecodeStream::append_varint(output, value_);
    }
  }

  void debug_print(std::string* output) override {
    output->append(StringPrintf("%d", value_));
  }

 private:
  int value_;
};

class BooleanConstant : public BooleanExpression {
 public:
  BooleanConstant(bool value) : value_(value) {}

  void serialize(std::string* output) override {
    BytecodeStream::append_opcode(output,
                                  value_ ? PUSH_CONSTANT_1 : PUSH_CONSTANT_0);
  }

  void debug_print(std::string* output) override {
    if (value_) {
      output->append("true");
    } else {
      output->append("false");
    }
  }

 private:
  bool value_;
};

class BoolAnd : public BooleanExpression {
 public:
  BoolAnd(std::shared_ptr<BooleanExpression> left,
          std::shared_ptr<BooleanExpression> right)
      : left_(std::move(left)), right_(std::move(right)) {
    HASSERT(left_);
    HASSERT(right_);
  }

  void serialize(std::string* output) override {
    left_->serialize(output);
    std::string rhs;
    BytecodeStream::append_opcode(&rhs, POP_OP);
    right_->serialize(&rhs);
    BytecodeStream::append_opcode(output, PUSH_TOP);
    BytecodeStream::append_opcode(output, TEST_JUMP_IF_FALSE);
    BytecodeStream::append_varint(output, rhs.size());
    output->append(rhs);
  }

  void debug_print(std::string* output) override {
    output->append("bool_and(");
    left_->debug_print(output);
    output->append(",");
    right_->debug_print(output);
    output->append(")");
  }

 private:
  std::shared_ptr<BooleanExpression> left_;
  std::shared_ptr<BooleanExpression> right_;
};

class BoolOr : public BooleanExpression {
 public:
  BoolOr(std::shared_ptr<BooleanExpression> left,
          std::shared_ptr<BooleanExpression> right)
      : left_(std::move(left)), right_(std::move(right)) {
    HASSERT(left_);
    HASSERT(right_);
  }

  void serialize(std::string* output) override {
    left_->serialize(output);
    std::string rhs;
    BytecodeStream::append_opcode(&rhs, POP_OP);
    right_->serialize(&rhs);
    BytecodeStream::append_opcode(output, PUSH_TOP);
    BytecodeStream::append_opcode(output, TEST_JUMP_IF_TRUE);
    BytecodeStream::append_varint(output, rhs.size());
    output->append(rhs);
  }

  void debug_print(std::string* output) override {
    output->append("bool_or(");
    left_->debug_print(output);
    output->append(",");
    right_->debug_print(output);
    output->append(")");
  }

 private:
  std::shared_ptr<BooleanExpression> left_;
  std::shared_ptr<BooleanExpression> right_;
};

} // namespace logic

#endif // _LOGIC_AST_HXX_
