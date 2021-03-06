#ifndef _bracz_train_automata_variables_hxx_
#define _bracz_train_automata_variables_hxx_

#include <string>
#include <memory>

using std::string;

#include "system.hxx"
#include "operations.hxx"
#include "registry.hxx"

#include "../cs/src/base.h"  // for constants in decodeoffset.

namespace automata {

// Creates the arguments for EventBasedVariable allocation from a contiguous
// counter.
inline bool DecodeOffset(int counter, int* client, int* offset, int* bit) {
  *bit = counter & 7;
  int autid = counter >> 3;
  *client = autid >> 3;
  int autofs = autid & 7;
  *offset = autofs * LEN_AUTOMATA + OFS_BITS;
  return (*client < 8);
}

class EventVariableBase : public GlobalVariable {
 public:
  EventVariableBase(Board* brd) {
    if (brd) {
      brd->AddVariable(this);
    }
  }

  void RenderHelper(string* output) {
    // We take the lead byte of op and the _ACT_DEF_VAR byte into account.
    SetId(output->size() + 2);

    vector<uint8_t> empty;
    vector<uint8_t> op;
    op.push_back(_ACT_DEF_VAR);
    op.push_back(arg1_);
    op.push_back(arg2_);
    Automata::Op::CreateOperation(output, empty, op);
  }

  // Creates an operation setting a particular eventid.
  //
  // dst is the index of eventid to set (0 or 1).
  static void CreateEventId(int dst, uint64_t eventid, string* output) {
    vector<uint8_t> empty;
    vector<uint8_t> op;
    op.push_back(_ACT_SET_EVENTID);
    if (dst) {
      op.push_back(0b01010111);
    } else {
      op.push_back(0b00000111);
    }
    for (int b = 56; b >= 0; b -= 8) {
      op.push_back((eventid >> b) & 0xff);
    }
    Automata::Op::CreateOperation(output, empty, op);
  }

 protected:
  uint8_t arg1_;
  uint8_t arg2_;
};

/**
   A global variable implementation that uses two events to set a state bit.
*/
class EventBasedVariable : public EventVariableBase {
 public:
  EventBasedVariable(Board* brd, const string& name, uint64_t event_on,
                     uint64_t event_off, int counter)
      : EventVariableBase(brd),
        event_on_(event_on),
        event_off_(event_off),
        name_(name) {
    int client, offset, bit;
    HASSERT(DecodeOffset(counter, &client, &offset, &bit));
    SetArgs(client, offset, bit);
    RegisterEventVariable(name, event_on, event_off);
  }

  EventBasedVariable(Board* brd, const string& name, uint64_t event_on,
                     uint64_t event_off, int client, int offset, int bit)
      : EventVariableBase(brd),
        event_on_(event_on),
        event_off_(event_off),
        name_(name) {
    SetArgs(client, offset, bit);
    RegisterEventVariable(name, event_on, event_off);
  }
  virtual ~EventBasedVariable() {}

  virtual uint64_t event_on() const { return event_on_; }
  virtual uint64_t event_off() const { return event_off_; }

  virtual void Render(string* output) {
    CreateEventId(1, event_on_, output);
    CreateEventId(0, event_off_, output);
    RenderHelper(output);
  }

  const string& name() const { return name_; }

 private:
  void SetArgs(int client, int offset, int bit) {
    arg1_ = (0 << 5) | (client & 0b11111);
    arg2_ = (offset << 3) | (bit & 7);
  }

  uint64_t event_on_, event_off_;
  string name_;
};

class AllocatorInterface;
typedef std::unique_ptr<AllocatorInterface> AllocatorPtr;

class AllocatorInterface {
 public:
  virtual ~AllocatorInterface();

  // Creates a new variable and transfers ownership to the caller.
  virtual GlobalVariable* Allocate(const string& name) const = 0;

  // Allocates a block and returns an allocator instance pointing to that
  // specific block.
  virtual AllocatorPtr Allocate(const string& name, int count,
                                int alignment = 1) const = 0;

  virtual const string& name() const = 0;

  virtual int remaining() const = 0;

  AllocatorPtr Forward(int num_to_reserve = 0) {
    HASSERT(num_to_reserve <= remaining());
    return Allocate("", remaining() - num_to_reserve);
  }

  virtual int TEST_Reserve(int num) { HASSERT(0); }

};

class UnionAllocator : public AllocatorInterface {
 public:
  UnionAllocator(std::initializer_list<const AllocatorInterface*> members)
      : members_(members) {
    HASSERT(members_.size());
  }

  const string& name() const override { return members_[0]->name(); }

  AllocatorPtr Allocate(const string& name, int count,
                        int alignment = 1) const override {
    CheckAdvance(name, count + alignment);
    return members_[next_]->Allocate(name, count, alignment);
  }

  GlobalVariable* Allocate(const string& name) const override {
    CheckAdvance(name, 1);
    return members_[next_]->Allocate(name);
  }

  int remaining() const override{
    return members_.back()->remaining();
  }

 private:
  // Checks that the current member has enough entries remaining for a given
  // allocation. If not, advances the member index. Dies if we run out of
  // members.
  void CheckAdvance(const string& name, int count) const {
    HASSERT(next_ < members_.size());
    if (count > members_[next_]->remaining()) {
      ++next_;
    }
    if (next_ >= members_.size()) {
      fprintf(stderr, "union-allocator %s ran out while trying to allocate %s",
              this->name().c_str(), name.c_str());
      HASSERT(0);
    }
  }

  std::vector<const AllocatorInterface*> members_;
  // Allocation may advance this member, hence volatile.
  mutable unsigned next_ = 0;
};


class EventBlock : public EventVariableBase {
 public:
  EventBlock(Board* brd, uint64_t event_base, const string& name,
             int size = 8 << 8)
      : EventVariableBase(brd),
        event_base_(event_base),
        size_(1),
        allocator_(new Allocator(this, name, size)) {}

  virtual void Render(string* output) {
    CreateEventId(0, event_base_, output);
    HASSERT(size_ < (8 << 8));
    arg1_ = (1 << 5) | ((size_ >> 8) & 7);
    arg2_ = size_ & 0xff;
    RenderHelper(output);
  }

  void SetMinSize(size_t size) {
    if (size >= size_) size_ = size + 1;
  }

  const AllocatorPtr& allocator() { return allocator_; }

  uint64_t event_base() const { return event_base_; }

  // These should never be called. This variable does not represent a single
  // bit.
  virtual uint64_t event_on() const { HASSERT(false); }
  virtual uint64_t event_off() const { HASSERT(false); }

 private:
  // Accesses the allocator directly.
  friend class BlockVariable;

  class Allocator : public AllocatorInterface {
   public:
    Allocator(EventBlock* block, const string& name, int size = 8 << 8)
        : name_(name), block_(block), next_entry_(0), end_(size) {}

    Allocator(const Allocator* parent, const string& name, int count,
              int alignment = 1)
        : name_(CreateNameFromParent(parent, name)), block_(parent->block_) {
      parent->Align(alignment);
      next_entry_ = parent->Reserve(count, name);
      end_ = next_entry_ + count;
    }

    Allocator(Allocator&& o) = delete;

    AllocatorPtr Allocate(const string& name, int count, int alignment = 1) const override {
      return AllocatorPtr(new Allocator(this, name, count, alignment));
    }

    int TEST_Reserve(int n) override {
      return Reserve(n);
    }

    // Reserves a number of entries at the beginning of the free block. Returns
    // the first entry that was reserved.
    int Reserve(int count, string caller = "unknown") const {
      if (next_entry_ + count > end_) {
        fprintf(stderr, "Allocator '%s' block overrun trying to reserve %d entries for '%s'.\n", name().c_str(), count, caller.c_str());
        HASSERT(0);
      }
      HASSERT(next_entry_ + count <= end_);
      int ret = next_entry_;
      next_entry_ += count;
      return ret;
    }

    // Creates a new variable and transfers ownership to the caller.
    GlobalVariable* Allocate(const string& name) const override;

    // Rounds up the next to-be-allocated entry to Alignment.
    void Align(int alignment) const {
      next_entry_ += alignment - 1;
      next_entry_ /= alignment;
      next_entry_ *= alignment;
    }

    const string& name() const override { return name_; }

    EventBlock* block() const { return block_; }

    int remaining() const { return end_ - next_entry_; }

    // Concatenates parent->name() and 'name', adding a '.' as separator if both
    // are non-empty.
    static string CreateNameFromParent(const Allocator* parent,
                                       const string& name) {
      if (name.empty()) return parent->name();
      if (parent->name().empty()) return name;
      string ret = parent->name();
      ret += ".";
      ret += name;
      return ret;
    }

   private:
    DISALLOW_COPY_AND_ASSIGN(Allocator);
    string name_;
    EventBlock* block_;
    mutable int next_entry_;
    int end_;
  };

  DISALLOW_COPY_AND_ASSIGN(EventBlock);
  uint64_t event_base_;
  uint16_t size_;
  AllocatorPtr allocator_;
};

class BlockVariable : public GlobalVariable {
 public:
  BlockVariable(const EventBlock::Allocator* allocator, const string& name)
      : parent_(allocator->block()),
        name_(allocator->CreateNameFromParent(allocator, name)) {
    int arg = allocator->Reserve(1, name);
    SetArg(arg);
    parent_->SetMinSize(arg);
    RegisterEventVariable(name_, event_on(), event_off());
  }

  virtual void Render(string* output) {
    // Block bits do not need any rendering. The render method will never be
    // called.
    HASSERT(0);
  }

  virtual GlobalVariableId GetId() const {
    GlobalVariableId tmp_id = parent_->GetId();
    tmp_id.arg = id_.arg;
    return tmp_id;
  }

  const string& name() { return name_; }

  virtual uint64_t event_on() const {
    return parent_->event_base() + id_.arg * 2;
  }

  virtual uint64_t event_off() const {
    return parent_->event_base() + id_.arg * 2 + 1;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(BlockVariable);
  EventBlock* parent_;
  string name_;
};

/**
   A global variable implementation that sets a signal variable to a given
value.  */
class SignalVariable : public EventVariableBase {
 public:
  SignalVariable(Board* brd, const string& name, uint64_t event_base,
                 uint8_t signal_id)
      : EventVariableBase(brd),
        event_base_(event_base),
        signal_id_(signal_id),
        name_(name) {
  }

  virtual ~SignalVariable() {}

  virtual uint64_t event_on() const { HASSERT(0); return 0; }
  virtual uint64_t event_off() const { HASSERT(0); return 0; }

  virtual void Render(string* output) {
    CreateEventId(0, event_base_, output);
    // @TODO(bracz) is 2 a free variable definition type?
    arg1_ = (2 << 5);  // We have some unused bits here.
    arg2_ = 2;  // size -- number of bytes to export.
    RenderHelper(output);
    // We use local id 30 to import the variable straight away. This will
    // override any other previous signal import, but that's fine, because we
    // are in the preamble and not in an automata.
    Automata::Op(nullptr, output).ActImportVariable(*this, 30);
    Automata::LocalVariable fixed_var(30);
    // We fix the first byte of the newly created variable to the signal ID.
    Automata::Op(nullptr, output).ActSetValue(&fixed_var, 0, signal_id_);
  }

  const string& name() const { return name_; }

 private:
  void SetArgs(int client, int offset, int bit) {
    arg1_ = (0 << 5) | (client & 0b11111);
    arg2_ = (offset << 3) | (bit & 7);
  }

  uint64_t event_base_;
  uint8_t signal_id_;
  string name_;
};

/**
   A global variable implementation that represents a single 8-bit value via
   256 consecutive events.  */
class ByteImportVariable : public EventVariableBase {
 public:
  ByteImportVariable(Board* brd, const string& name, uint64_t event_base,
                     uint8_t default_value)
      : EventVariableBase(brd),
        event_base_(event_base),
        default_value_(default_value),
        name_(name) {}

  virtual uint64_t event_on() const { HASSERT(0); return 0; }
  virtual uint64_t event_off() const { HASSERT(0); return 0; }

  virtual void Render(string* output) {
    CreateEventId(0, event_base_, output);
    arg1_ = (3 << 5);  // Event byte block consumer
    arg2_ = 1;  // size -- number of bytes to import.
    RenderHelper(output);
    // We use local id 30 to import the variable straight away. This will
    // override any other previous signal import, but that's fine, because we
    // are in the preamble and not in an automata.
    Automata::Op(nullptr, output).ActImportVariable(*this, 30);
    Automata::LocalVariable fixed_var(30);
    // We fix the first byte of the newly created variable to the default.
    Automata::Op(nullptr, output).ActSetValue(&fixed_var, 0, default_value_);
  }

  const string& name() const { return name_; }

 private:
  uint64_t event_base_;
  uint8_t signal_id_;
  uint8_t default_value_;
  string name_;
};

void ClearEventMap();
map<uint64_t, string>* GetEventMap();

}  // namespace

#endif  // _bracz_train_automata_variables_hxx_
