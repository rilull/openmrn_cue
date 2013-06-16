#ifndef _bracz_train_automata_system_hxx_
#define _bracz_train_automata_system_hxx_

#include <assert.h>

#include <string>
#include <vector>
#include <map>
using std::string;
using std::vector;
using std::map;

namespace automata {

//! Put this into the private section of a class to prevent the default copy
//! constructors to be generated.
#define DISALLOW_COPY_AND_ASSIGN(Name) Name& operator=(Name&); Name(Name&)

class Automata;
class GlobalVariable;

/** This class represents an entire board and contains all the bit definitions
    and all automatas that this board contains. This class is able to
    eventually output a bytestream for the compiled automata.
 */
class Board {
public:
    Board() {}
    ~Board();

    //! Generates the binary data for the entire board.
    void Render(string* output);

    void AddAutomata(Automata* a) {
        automatas_.push_back(AutomataInfo(*a));
    }

    void AddVariable(GlobalVariable* v) {
        global_variables_.push_back(v);
    }

private:
    void RenderPreamble(string* output);
    void RenderAutomatas(string* output);

    struct AutomataInfo {
        AutomataInfo(Automata& a)
            : automata(&a), ptr_offset(-1), offset(-1) {}
        ~AutomataInfo() { }
        Automata* automata;
        int ptr_offset; //< Offset in the binary to the pointer to the automata.
        int offset; //< This is the code's offset in the binary.
    };

    vector<AutomataInfo> automatas_;
    vector<GlobalVariable*> global_variables_;

    DISALLOW_COPY_AND_ASSIGN(Board);
};

struct GlobalVariableId {
public:
    uint32_t id;
};

/** Represents a single bit that the automata runner system will track. This
    variable is not assigned to any automata and will create an entry in the
    preamble.
*/
class GlobalVariable {
public:
    GlobalVariable()
        : id_assigned_(false) {}
    virtual ~GlobalVariable() {}
    GlobalVariableId GetId() {
        assert(id_assigned_);
        return id_;
    }
    void SetId(uint32_t id) {
        id_assigned_ = true;
        id_.id = id;
    }
    //! Appends the rendered version of this automata to the preamble of the 
    virtual void Render(string* output) = 0;

private:
    bool id_assigned_;
    GlobalVariableId id_;
    DISALLOW_COPY_AND_ASSIGN(GlobalVariable);
};


/** Represents a concrete automata in the current board.
 */
class Automata {
public:
    Automata() : timer_bit_(0), output_(NULL) {
        // We add the timer variable to the map with a fake key in order to
        // reserve local bit 0.
        used_variables_[NULL] = timer_bit_;
    }
    ~Automata() {}

    void Render(string* output);

    class Op;

protected:
    virtual void Body() = 0;

    struct LocalVariable {
    public:
        LocalVariable(): id(-1) {}
        LocalVariable(int iid): id(iid) {}
        int GetId() {
            assert(id >= 0 && id < 32);
            return id;
        }
        int id;
        // We purposefully allow this object to be copied.
    };

    LocalVariable& ImportVariable(GlobalVariable* var);

    LocalVariable timer_bit_;

#define Def() Op(this, output_)

    string* output_;
private:
    friend class Op;

    map<GlobalVariable*, LocalVariable> used_variables_;
    DISALLOW_COPY_AND_ASSIGN(Automata);
};

#define DefAut(name, boardref, body) class Aut##name : public Automata {public:  Aut##name() {(boardref).AddAutomata(this); } protected: virtual void Body() body } name##instance

}

#endif // _bracz_train_automata_system_hxx_