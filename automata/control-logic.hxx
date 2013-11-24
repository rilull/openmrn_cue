// Declarations of unified structures used for control point logic, such as
// exlusive locks for blocks and turnouts, ABS, occupancy generation, etc.

#ifndef _AUTOMATA_CONTROL_LOGIC_HXX_
#define _AUTOMATA_CONTROL_LOGIC_HXX_

#include <map>
#include <memory>
#include <algorithm>

#include "system.hxx"
#include "operations.hxx"
#include "variables.hxx"
#include "gtest/gtest_prod.h"

class StandardBlock;

namespace automata {

extern StateRef StInit, StBase;

void HandleInitState(Automata* aut);

struct PhysicalSignal {
  PhysicalSignal(const GlobalVariable* sensor, GlobalVariable* signal)
      : sensor_raw(sensor), signal_raw(signal) {}
  const GlobalVariable* sensor_raw;
  GlobalVariable* signal_raw;
};

typedef Callback1<Automata*> AutomataCallback;

// This class allows registering multiple feature callbacks that will all be
// applied to an automata in one go, in user-specified ordering.
class AutomataPlugin {
 public:
  AutomataPlugin() {
    AddAutomataPlugin(10000, NewCallbackPtr(&HandleInitState));
  }
  virtual ~AutomataPlugin() {
    for (const auto& it : cb_) {
      delete it.second;
    }
  }

  void RunAll(Automata* aut) {
    for (const auto& it : cb_) {
      it.second->Run(aut);
    }
  }

  // The plugins will be called in increasing order of n.
  void AddAutomataPlugin(int n, AutomataCallback* cb) {
    cb_.insert(std::make_pair(n, cb));
  }

  // Removes all plugins with the given priority.
  void RemoveAutomataPlugins(int n) {
    auto range = cb_.equal_range(n);
    for (auto r = range.first; r != range.second; r++) {
      delete r->second;
    }
    cb_.erase(range.first, range.second);
  }

 private:
  std::multimap<int, AutomataCallback*> cb_;
};

// A straight automata definition that calls all the plugins.
class StandardPluginAutomata : public automata::Automata {
 public:
  StandardPluginAutomata(Board* brd, AutomataPlugin* plugins)
      : plugins_(plugins), brd_(brd) {
    brd->AddAutomata(this);
  }

  virtual void Body() {
    plugins_->RunAll(this);
  }

  virtual Board* board() { return brd_; }

 private:
  AutomataPlugin* plugins_;
  Board* brd_;
};

class CtrlTrackInterface;

// This class is the basis of sequential binding. It defines a run of track
// which has exactly two endpoints. Internally it might be made of one or more
// track pieces that are already bound.
class StraightTrackInterface {
 public:
  virtual CtrlTrackInterface* side_a() = 0;
  virtual CtrlTrackInterface* side_b() = 0;
};

// Bind together a sequence of straight tracks. The return value is always
// true.
bool BindSequence(const std::initializer_list<StraightTrackInterface*>& pieces);

// Interface for finding real occupancy detectors at a track piece.
class OccupancyLookupInterface {
public:
  virtual ~OccupancyLookupInterface() {}

  // Returns the first (real) occupancy detector within train length from the
  // interface `from'. Returns NULL if no such detector was found. The interface is interpreted by
  virtual const GlobalVariable* LookupCloseDetector(
      const CtrlTrackInterface* from) = 0;

  // Returns the first occupancy detector outside of train length from the
  // interface `from'. Returns NULL if no such detector was found.
  virtual const GlobalVariable* LookupFarDetector(
      const CtrlTrackInterface* from) = 0;
};

class CtrlTrackInterface {
 public:
  CtrlTrackInterface(const EventBlock::Allocator& allocator, OccupancyLookupInterface* parent)
      : out_try_set_route(allocator.Allocate("out_try_set_route")),
        in_route_set_success(allocator.Allocate("in_route_set_success")),
        in_route_set_failure(allocator.Allocate("in_route_set_failure")),
        out_route_released(allocator.Allocate("out_route_released")),
        lookup_if_(parent),
        binding_(nullptr) {}


  std::unique_ptr<GlobalVariable> out_try_set_route;
  std::unique_ptr<GlobalVariable> in_route_set_success;
  std::unique_ptr<GlobalVariable> in_route_set_failure;
  std::unique_ptr<GlobalVariable> out_route_released;

  const GlobalVariable* LookupNextDetector() const {
    const GlobalVariable* ret = LookupCloseDetector();
    if (ret != nullptr) {
      return ret;
    }
    ret = LookupFarDetector();
    if (ret != nullptr) {
      return ret;
    }
    extern bool could_not_retrieve_next_occupancy_variable();
    HASSERT(false && could_not_retrieve_next_occupancy_variable());
  }

  // Occupancy value of the current block, or the neighboring block less than a
  // train-length away.
  const GlobalVariable* LookupCloseDetector() const {
    return lookup_if_->LookupCloseDetector(this);
  }
  // Occupancy value of the block that is more than train-length away.
  const GlobalVariable* LookupFarDetector() const {
    return lookup_if_->LookupFarDetector(this);
  }

  CtrlTrackInterface* binding() const {
    return binding_;
  }

  void Bind(CtrlTrackInterface* other) {
    binding_ = other;
    other->binding_ = this;
  }

 private:
  // Where to ask for occupancy lookups.
  OccupancyLookupInterface* lookup_if_;

  // The other interface connected to this track end.
  CtrlTrackInterface* binding_;
};

void SimulateOccupancy(Automata* aut,
                       Automata::LocalVariable* sim_occ,
                       Automata::LocalVariable* tmp_seen_train_in_next,
                       const Automata::LocalVariable& route_set,
                       CtrlTrackInterface* past_if,
                       CtrlTrackInterface* next_if,\
                       OpCallback* release_cb);

class StraightTrack : public StraightTrackInterface,
                      public OccupancyLookupInterface,
                      public virtual AutomataPlugin {
 public:
  StraightTrack(const EventBlock::Allocator& allocator)
      : side_a_(EventBlock::Allocator(&allocator, "a", 8), this),
        side_b_(EventBlock::Allocator(&allocator, "b", 8), this),
        simulated_occupancy_(allocator.Allocate("simulated_occ")),
        route_set_ab_(allocator.Allocate("route_set_ab")),
        route_set_ba_(allocator.Allocate("route_set_ba")),
        route_pending_ab_(allocator.Allocate("route_pending_ab")),
        route_pending_ba_(allocator.Allocate("route_pending_ba")),
        tmp_seen_train_in_next_(allocator.Allocate("tmp_seen_train_in_next")),
        tmp_route_setting_in_progress_(allocator.Allocate("tmp_route_setting_in_progress"))
  {
    AddAutomataPlugin(20, NewCallbackPtr(
        this, &StraightTrack::SimulateAllOccupancy));
  }

  virtual CtrlTrackInterface* side_a() { return &side_a_; }
  virtual CtrlTrackInterface* side_b() { return &side_b_; }

  void SimulateAllOccupancy(Automata* aut);
  void SimulateAllRoutes(Automata* aut);

 protected:
  void Bind(CtrlTrackInterface* me, CtrlTrackInterface* opposite);

  CtrlTrackInterface side_a_;
  CtrlTrackInterface side_b_;

  // If you give this side_a() it will return side_b() and vice versa.
  const CtrlTrackInterface* FindOtherSide(const CtrlTrackInterface* s);

  FRIEND_TEST(LogicTest, SimulatedOccupancy_SingleShortPiece);
  FRIEND_TEST(LogicTest, SimulatedOccupancy_MultipleShortPiece);
  FRIEND_TEST(LogicTest, SimulatedOccupancy_ShortAndLongPieces);
  FRIEND_TEST(LogicTest, SimulatedOccupancy_RouteSetting);
  FRIEND_TEST(LogicTest, SimulatedOccupancy_SimultSetting);
  FRIEND_TEST(LogicTest, ReverseRoute);
  FRIEND_TEST(LogicTest, MultiRoute);
  FRIEND_TEST(LogicTest, Signal);
  FRIEND_TEST(LogicTest, 100trainz);

  friend class ::StandardBlock;

  std::unique_ptr<GlobalVariable> simulated_occupancy_;
  // route from A [in] to B [out]
  std::unique_ptr<GlobalVariable> route_set_ab_;
  // route from B [in] to A [out]
  std::unique_ptr<GlobalVariable> route_set_ba_;
  // temp var for route AB
  std::unique_ptr<GlobalVariable> route_pending_ab_;
  // temp var for route BA
  std::unique_ptr<GlobalVariable> route_pending_ba_;
  // Helper variable for simuating occupancy.
  std::unique_ptr<GlobalVariable> tmp_seen_train_in_next_;
  // Helper variable for excluding parallel route setting requests.
  std::unique_ptr<GlobalVariable> tmp_route_setting_in_progress_;
};

class StraightTrackWithDetector : public StraightTrack {
public:
  StraightTrackWithDetector(const EventBlock::Allocator& allocator,
                            GlobalVariable* detector)
    : StraightTrack(allocator),
      detector_(detector) {
    // No occupancy simulation needed.
    RemoveAutomataPlugins(20);
    AddAutomataPlugin(20, NewCallbackPtr(
        this, &StraightTrackWithDetector::DetectorOccupancy));
    AddAutomataPlugin(30, NewCallbackPtr(
        this, &StraightTrackWithDetector::DetectorRoute));
  }

  virtual const GlobalVariable* LookupCloseDetector(
      const CtrlTrackInterface* from) {
    return detector_;
  }

  // Returns the first occupancy detector outside of train length from the
  // interface `from'. Returns NULL if no such detector was found.
  virtual const GlobalVariable* LookupFarDetector(
      const CtrlTrackInterface* from) {
    // TODO(bracz): this should actually be propagated instead of NULLing.
    return NULL;
  }

protected:
  void DetectorRoute(Automata* aut);
  void DetectorOccupancy(Automata* aut);

  GlobalVariable* detector_;
};

class StraightTrackWithRawDetector : public StraightTrackWithDetector {
public:
  StraightTrackWithRawDetector(const EventBlock::Allocator& allocator,
                               const GlobalVariable* raw_detector)
      : StraightTrackWithDetector(allocator, nullptr),
        raw_detector_(raw_detector),
        debounce_temp_var_(allocator.Allocate("tmp_debounce")) {
    detector_ = simulated_occupancy_.get();
    RemoveAutomataPlugins(20);
    AddAutomataPlugin(20, NewCallbackPtr(
        this, &StraightTrackWithRawDetector::RawDetectorOccupancy));
  }

protected:
  void RawDetectorOccupancy(Automata* aut);

  const GlobalVariable* raw_detector_;
  std::unique_ptr<GlobalVariable> debounce_temp_var_;
};

class StraightTrackShort : public StraightTrack {
public:
  StraightTrackShort(const EventBlock::Allocator& allocator)
    : StraightTrack(allocator) {
    AddAutomataPlugin(30, NewCallbackPtr(
        (StraightTrack*)this, &StraightTrack::SimulateAllRoutes));
  }

  virtual const GlobalVariable* LookupCloseDetector(
      const CtrlTrackInterface* from) {
    return FindOtherSide(from)->binding()->LookupCloseDetector();
  }

  // Returns the first occupancy detector outside of train length from the
  // interface `from'. Returns NULL if no such detector was found.
  virtual const GlobalVariable* LookupFarDetector(
      const CtrlTrackInterface* from) {
    return FindOtherSide(from)->binding()->LookupFarDetector();
  }
};

class StraightTrackLong : public StraightTrack {
public:
  StraightTrackLong(const EventBlock::Allocator& allocator)
    : StraightTrack(allocator) {
    AddAutomataPlugin(30, NewCallbackPtr(
        this, &StraightTrack::SimulateAllRoutes));
  }

  virtual const GlobalVariable* LookupCloseDetector(
      const CtrlTrackInterface* from) {
    return nullptr;
  }

  // Returns the first occupancy detector outside of train length from the
  // interface `from'. Returns NULL if no such detector was found.
  virtual const GlobalVariable* LookupFarDetector(
      const CtrlTrackInterface* from) {
    return FindOtherSide(from)->binding()->LookupNextDetector();
  }
};

// A signal is a piece where trains coming from side_a towards side_b can (and
// shall) stop if the signal is red.
class SignalPiece : public StraightTrackShort {
 public:
  SignalPiece(const EventBlock::Allocator& allocator,
              GlobalVariable* request_green,
              GlobalVariable* signal)
      : StraightTrackShort(allocator),
        request_green_(request_green),
        signal_(signal) {
    // We "override" the occupancy detection by just copying the occupancy
    // value from the previous piece of track.
    RemoveAutomataPlugins(20);
    AddAutomataPlugin(20, NewCallbackPtr(
        this, &SignalPiece::SignalOccupancy));
    // Signals have custom route setting logic in the forward direction.
    RemoveAutomataPlugins(30);
    AddAutomataPlugin(30, NewCallbackPtr(
        this, &SignalPiece::SignalRoute));
  }

  virtual const GlobalVariable* LookupCloseDetector(
      const CtrlTrackInterface* from) {
    return FindOtherSide(from)->binding()->LookupCloseDetector();
  }

  // Returns the first occupancy detector outside of train length from the
  // interface `from'. Returns NULL if no such detector was found.
  virtual const GlobalVariable* LookupFarDetector(
      const CtrlTrackInterface* from) {
    return FindOtherSide(from)->binding()->LookupFarDetector();
  }

  void SignalRoute(Automata* aut);
  void SignalOccupancy(Automata* aut);

 private:
  GlobalVariable* request_green_;
  GlobalVariable* signal_;
};


class TurnoutInterface {
 public:
  virtual CtrlTrackInterface* side_points() = 0;
  virtual CtrlTrackInterface* side_closed() = 0;
  virtual CtrlTrackInterface* side_thrown() = 0;
};

class TurnoutBase : public TurnoutInterface, private OccupancyLookupInterface, public virtual AutomataPlugin {
 public:
  TurnoutBase(const EventBlock::Allocator& allocator)
      : side_points_(EventBlock::Allocator(&allocator, "points", 8), this),
        side_closed_(EventBlock::Allocator(&allocator, "closed", 8), this),
        side_thrown_(EventBlock::Allocator(&allocator, "thrown", 8), this),
        simulated_occupancy_(allocator.Allocate("simulated_occ")),
        route_set_PC_(allocator.Allocate("route_set_PC")),
        route_set_CP_(allocator.Allocate("route_set_CP")),
        route_set_PT_(allocator.Allocate("route_set_PT")),
        route_set_TP_(allocator.Allocate("route_set_TP")),
        route_pending_PC_(allocator.Allocate("route_pending_PC")),
        route_pending_CP_(allocator.Allocate("route_pending_CP")),
        route_pending_PT_(allocator.Allocate("route_pending_PT")),
        route_pending_TP_(allocator.Allocate("route_pending_TP")),
        any_route_set_(allocator.Allocate("any_route_set")),
        turnout_state_(allocator.Allocate("turnout_state")),
        tmp_seen_train_in_next_(allocator.Allocate("tmp_seen_train_in_next")),
        tmp_route_setting_in_progress_(allocator.Allocate("tmp_route_setting_in_progress"))
  {
    directions_.push_back(Direction(&side_points_, &side_closed_, route_set_PC_.get(), route_pending_PC_.get()));
    directions_.push_back(Direction(&side_points_, &side_thrown_, route_set_PT_.get(), route_pending_PT_.get()));
    directions_.push_back(Direction(&side_closed_, &side_points_, route_set_CP_.get(), route_pending_CP_.get()));
    directions_.push_back(Direction(&side_thrown_, &side_points_, route_set_TP_.get(), route_pending_TP_.get()));
    AddAutomataPlugin(20, NewCallbackPtr(this, &TurnoutBase::TurnoutOccupancy));
    AddAutomataPlugin(30, NewCallbackPtr(this, &TurnoutBase::TurnoutRoute));
    AddAutomataPlugin(35, NewCallbackPtr(this, &TurnoutBase::PopulateAnyRouteSet));
  }

  virtual CtrlTrackInterface* side_points() {
    return &side_points_;
  }
  virtual CtrlTrackInterface* side_closed() {
    return &side_closed_;
  }
  virtual CtrlTrackInterface* side_thrown() {
    return &side_thrown_;
  }
  
 protected:
  CtrlTrackInterface side_points_;
  CtrlTrackInterface side_closed_;
  CtrlTrackInterface side_thrown_;

  struct Direction {
    Direction(CtrlTrackInterface* f,
              CtrlTrackInterface* t,
              GlobalVariable* r,
              GlobalVariable* rp)
        : from(f), to(t), route(r), route_pending(rp) {}
    CtrlTrackInterface* from;
    CtrlTrackInterface* to;
    GlobalVariable* route;
    GlobalVariable* route_pending;
  };

  vector<Direction> directions_;

  std::unique_ptr<GlobalVariable> simulated_occupancy_;
  // route from closed/thrown/points [in] to closed/thrown/points [out]
  std::unique_ptr<GlobalVariable> route_set_PC_;
  std::unique_ptr<GlobalVariable> route_set_CP_;
  std::unique_ptr<GlobalVariable> route_set_PT_;
  std::unique_ptr<GlobalVariable> route_set_TP_;
  // temp var for routes
  std::unique_ptr<GlobalVariable> route_pending_PC_;
  std::unique_ptr<GlobalVariable> route_pending_CP_;
  std::unique_ptr<GlobalVariable> route_pending_PT_;
  std::unique_ptr<GlobalVariable> route_pending_TP_;

  // Helper variable that is 1 iff any of the four routes are set.
  std::unique_ptr<GlobalVariable> any_route_set_;

  // Is zero if turnout is closed, 1 if turnout is thrown.
  std::unique_ptr<GlobalVariable> turnout_state_;

  // Helper variable for simuating occupancy.
  std::unique_ptr<GlobalVariable> tmp_seen_train_in_next_;
  // Helper variable for excluding parallel route setting requests.
  std::unique_ptr<GlobalVariable> tmp_route_setting_in_progress_;

 private:
  void PopulateAnyRouteSet(Automata* aut);
  void TurnoutOccupancy(Automata* aut);
  void TurnoutRoute(Automata* aut);
};

class FixedTurnout : public TurnoutBase {
 public:
  enum State {
    TURNOUT_CLOSED,
    TURNOUT_THROWN
  };

  FixedTurnout(State state, const EventBlock::Allocator& allocator)
      : TurnoutBase(allocator) {
    AddAutomataPlugin(10, NewCallbackPtr(this, &FixedTurnout::FixTurnoutState));
  }

  virtual const GlobalVariable* LookupCloseDetector(
      const CtrlTrackInterface* from) {
    return FindOtherSide(from)->binding()->LookupCloseDetector();
  }

  virtual const GlobalVariable* LookupFarDetector(
      const CtrlTrackInterface* from) {
    return FindOtherSide(from)->binding()->LookupFarDetector();
  }

 private:
  void FixTurnoutState(Automata* aut);

  State state_;
  
  const CtrlTrackInterface* FindOtherSide(const CtrlTrackInterface* s) {
    if (s == &side_closed_) return &side_points_;
    if (s == &side_thrown_) return &side_points_;
    if (s == &side_points_ &&
        state_ == TURNOUT_CLOSED) return &side_closed_;
    if (s == &side_points_ &&
        state_ == TURNOUT_THROWN) return &side_thrown_;
    assert(0);
    return nullptr;
  }
};

}  // namespace automata

#endif // _AUTOMATA_CONTROL_LOGIC_HXX_
