// Declarations of unified structures used for control point logic, such as
// exlusive locks for blocks and turnouts, ABS, occupancy generation, etc.

#ifndef _AUTOMATA_CONTROL_LOGIC_HXX_
#define _AUTOMATA_CONTROL_LOGIC_HXX_

#include <map>
#include <memory>
#include <algorithm>
#include <cstddef>
#include <stdint.h>

#include "system.hxx"
#include "operations.hxx"
#include "variables.hxx"
#include "gtest/gtest_prod.h"

namespace automata {

static constexpr StateRef StInit(0);
static constexpr StateRef StBase(1);

extern GlobalVariable* reset_routes;

void HandleInitState(Automata *aut);

struct PhysicalSignal {
  PhysicalSignal(const GlobalVariable *sensor, GlobalVariable *signal,
                 SignalVariable *main_sgn, SignalVariable *adv_sgn,
                 SignalVariable *r_main_sgn, SignalVariable *r_adv_sgn,
                 SignalVariable *in_adv_sgn, SignalVariable *r_in_adv_sgn)
      : sensor_raw(sensor),
        signal_raw(signal),
        main_sgn(main_sgn),
        adv_sgn(adv_sgn),
        r_main_sgn(r_main_sgn),
        r_adv_sgn(r_adv_sgn),
        in_adv_sgn(in_adv_sgn),
        r_in_adv_sgn(r_in_adv_sgn) {}
  const GlobalVariable *sensor_raw;
  GlobalVariable *signal_raw;
  SignalVariable *main_sgn;
  SignalVariable *adv_sgn;
  SignalVariable *r_main_sgn;
  SignalVariable *r_adv_sgn;
  SignalVariable *in_adv_sgn;
  SignalVariable *r_in_adv_sgn;
};

typedef Callback1<Automata *> AutomataCallback;

// This class allows registering multiple feature callbacks that will all be
// applied to an automata in one go, in user-specified ordering.
class AutomataPlugin {
 public:
  AutomataPlugin() {
    AddAutomataPlugin(10000, NewCallbackPtr(&HandleInitState));
  }
  virtual ~AutomataPlugin() {
    for (const auto &it : cb_) {
      delete it.second;
    }
  }

  void RunAll(Automata *aut) {
    for (const auto &it : cb_) {
      it.second->Run(aut);
    }
  }

  // The plugins will be called in increasing order of n.
  void AddAutomataPlugin(int n, AutomataCallback *cb) {
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
  std::multimap<int, AutomataCallback *> cb_;
};

// A straight automata definition that calls all the plugins.
class StandardPluginAutomata : public automata::Automata {
 public:
  StandardPluginAutomata(const string &name, Board *brd,
                         AutomataPlugin *plugins)
      : automata::Automata(name), plugins_(plugins), brd_(brd) {
    brd->AddAutomata(this);
  }

  virtual void Body() { plugins_->RunAll(this); }

  virtual Board *board() { return brd_; }

 private:
  AutomataPlugin *plugins_;
  Board *brd_;
};

class MagnetCommandAutomata;

struct MagnetDef {
  MagnetDef(MagnetCommandAutomata *aut, const string &name,
            GlobalVariable *closed, GlobalVariable *thrown);
  // This variable will be pulled when the command is zero (closed).
  GlobalVariable *set_0;
  // This variable will be pulled when the command is one (thrown).
  GlobalVariable *set_1;

  // Thnese variables are allocated by the magnetcommandautomata.

  // This variable sets the commanded state.
  std::unique_ptr<GlobalVariable> command;
  // This variable tracks the current state.
  std::unique_ptr<GlobalVariable> current_state;
  // True if the current state must not be changed
  std::unique_ptr<GlobalVariable> locked;
  // Initialized by the magnetcommandautomata.
  StateRef aut_state;
  string name_;
};

class MagnetCommandAutomata : public virtual AutomataPlugin {
 public:
  MagnetCommandAutomata(Board *brd, const EventBlock::Allocator &alloc);

  // Adds a magnet to this automata.
  void AddMagnet(MagnetDef *def);

  int NewUserState() {
    return aut_.NewUserState();
  }

 private:
  EventBlock::Allocator alloc_;

  StandardPluginAutomata aut_;
};

class CtrlTrackInterface;

// This class is the basis of sequential binding. It defines a run of track
// which has exactly two endpoints. Internally it might be made of one or more
// track pieces that are already bound.
class StraightTrackInterface {
 public:
  virtual CtrlTrackInterface *side_a() = 0;
  virtual CtrlTrackInterface *side_b() = 0;
};

// Bind together a sequence of straight tracks. The return value is always
// true.
bool BindSequence(
    const std::initializer_list<StraightTrackInterface *> &pieces);

// Binds together pairs of interfaces. The return value is always true.
bool BindPairs(const std::initializer_list<
    std::initializer_list<CtrlTrackInterface *> > &pieces);

// Interface for finding real occupancy detectors at a track piece.
class OccupancyLookupInterface {
 public:
  virtual ~OccupancyLookupInterface() {}

  // Returns the first (real) occupancy detector within train length from the
  // interface `from'. Returns NULL if no such detector was found. The interface
  // is interpreted by
  virtual const GlobalVariable *LookupCloseDetector(
      const CtrlTrackInterface *from) = 0;

  // Returns the first occupancy detector outside of train length from the
  // interface `from'. Returns NULL if no such detector was found.
  virtual const GlobalVariable *LookupFarDetector(
      const CtrlTrackInterface *from) = 0;
};

class CtrlTrackInterface {
 public:
  CtrlTrackInterface(const EventBlock::Allocator &allocator,
                     OccupancyLookupInterface *parent)
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

  const GlobalVariable *LookupNextDetector() const {
    const GlobalVariable *ret = LookupCloseDetector();
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
  const GlobalVariable *LookupCloseDetector() const {
    return lookup_if_->LookupCloseDetector(this);
  }
  // Occupancy value of the block that is more than train-length away.
  const GlobalVariable *LookupFarDetector() const {
    return lookup_if_->LookupFarDetector(this);
  }

  CtrlTrackInterface *binding() const { return binding_; }

  bool Bind(CtrlTrackInterface *other) {
    binding_ = other;
    other->binding_ = this;
    return true;
  }

 private:
  // Where to ask for occupancy lookups.
  OccupancyLookupInterface *lookup_if_;

  // The other interface connected to this track end.
  CtrlTrackInterface *binding_;
};

void SimulateOccupancy(Automata *aut, Automata::LocalVariable *sim_occ,
                       Automata::LocalVariable *tmp_seen_train_in_next,
                       const Automata::LocalVariable &route_set,
                       CtrlTrackInterface *past_if, CtrlTrackInterface *next_if,
                       OpCallback *release_cb);

class StraightTrack : public StraightTrackInterface,
                      public OccupancyLookupInterface,
                      public virtual AutomataPlugin {
 public:
  StraightTrack(const EventBlock::Allocator &allocator)
      : side_a_(EventBlock::Allocator(&allocator, "a", 8), this),
        side_b_(EventBlock::Allocator(&allocator, "b", 8), this),
        simulated_occupancy_(allocator.Allocate("simulated_occ")),
        route_set_ab_(allocator.Allocate("route_set_ab")),
        route_set_ba_(allocator.Allocate("route_set_ba")),
        route_pending_ab_(allocator.Allocate("route_pending_ab")),
        route_pending_ba_(allocator.Allocate("route_pending_ba")),
        tmp_seen_train_in_next_(allocator.Allocate("tmp_seen_train_in_next")),
        tmp_route_setting_in_progress_(
            allocator.Allocate("tmp_route_setting_in_progress")) {
    AddAutomataPlugin(
        20, NewCallbackPtr(this, &StraightTrack::SimulateAllOccupancy));
  }

  virtual CtrlTrackInterface *side_a() { return &side_a_; }
  virtual CtrlTrackInterface *side_b() { return &side_b_; }

  void SimulateAllOccupancy(Automata *aut);
  void SimulateAllRoutes(Automata *aut);

 protected:
  bool Bind(CtrlTrackInterface *me, CtrlTrackInterface *opposite);

  CtrlTrackInterface side_a_;
  CtrlTrackInterface side_b_;

  // If you give this side_a() it will return side_b() and vice versa.
  const CtrlTrackInterface *FindOtherSide(const CtrlTrackInterface *s);

  FRIEND_TEST(LogicTest, SimulatedOccupancy_SingleShortPiece);
  FRIEND_TEST(LogicTest, SimulatedOccupancy_MultipleShortPiece);
  FRIEND_TEST(LogicTest, SimulatedOccupancy_ShortAndLongPieces);
  FRIEND_TEST(LogicTest, SimulatedOccupancy_RouteSetting);
  FRIEND_TEST(LogicTest, SimulatedOccupancy_SimultSetting);
  FRIEND_TEST(LogicTest, ReverseRoute);
  FRIEND_TEST(LogicTest, MultiRoute);
  FRIEND_TEST(LogicTest, Signal0);
  FRIEND_TEST(LogicTest, Signal);
  FRIEND_TEST(LogicTest, 100trainz);
  FRIEND_TEST(LogicTest, DISABLED_100trainz);
  FRIEND_TEST(LogicTest, FixedTurnout);
  FRIEND_TEST(LogicTest, MovableTurnout);
  FRIEND_TEST(LogicTrainTest, ScheduleStraight);
  FRIEND_TEST(SampleLayoutLogicTrainTest, ScheduleStraight);
  FRIEND_TEST(SampleLayoutLogicTrainTest, ScheduleConditional);

  friend class StandardBlock;

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
  StraightTrackWithDetector(const EventBlock::Allocator &allocator,
                            GlobalVariable *detector)
      : StraightTrack(allocator), detector_(detector) {
    // No occupancy simulation needed.
    RemoveAutomataPlugins(20);
    AddAutomataPlugin(
        20,
        NewCallbackPtr(this, &StraightTrackWithDetector::DetectorOccupancy));
    AddAutomataPlugin(
        30, NewCallbackPtr(this, &StraightTrackWithDetector::DetectorRoute));
  }

  virtual const GlobalVariable *LookupCloseDetector(
      const CtrlTrackInterface *from) {
    return detector_;
  }

  // Returns the first occupancy detector outside of train length from the
  // interface `from'. Returns NULL if no such detector was found.
  virtual const GlobalVariable *LookupFarDetector(
      const CtrlTrackInterface *from) {
    // TODO(bracz): this should actually be propagated instead of NULLing.
    return NULL;
  }

 protected:
  void DetectorRoute(Automata *aut);
  void DetectorOccupancy(Automata *aut);

  GlobalVariable *detector_;
};

class StraightTrackWithRawDetector : public StraightTrackWithDetector {
 public:
  StraightTrackWithRawDetector(const EventBlock::Allocator &allocator,
                               const GlobalVariable *raw_detector)
      : StraightTrackWithDetector(allocator, nullptr),
        raw_detector_(raw_detector),
        debounce_temp_var_(allocator.Allocate("tmp_debounce")) {
    detector_ = simulated_occupancy_.get();
    RemoveAutomataPlugins(20);
    AddAutomataPlugin(
        20, NewCallbackPtr(
                this, &StraightTrackWithRawDetector::RawDetectorOccupancy));
  }

 protected:
  void RawDetectorOccupancy(Automata *aut);

  const GlobalVariable *raw_detector_;
  std::unique_ptr<GlobalVariable> debounce_temp_var_;
};

class StraightTrackShort : public StraightTrack {
 public:
  StraightTrackShort(const EventBlock::Allocator &allocator)
      : StraightTrack(allocator) {
    AddAutomataPlugin(30, NewCallbackPtr((StraightTrack *)this,
                                         &StraightTrack::SimulateAllRoutes));
  }

  virtual const GlobalVariable *LookupCloseDetector(
      const CtrlTrackInterface *from) {
    return FindOtherSide(from)->binding()->LookupCloseDetector();
  }

  // Returns the first occupancy detector outside of train length from the
  // interface `from'. Returns NULL if no such detector was found.
  virtual const GlobalVariable *LookupFarDetector(
      const CtrlTrackInterface *from) {
    return FindOtherSide(from)->binding()->LookupFarDetector();
  }
};

class StraightTrackLong : public StraightTrack {
 public:
  StraightTrackLong(const EventBlock::Allocator &allocator)
      : StraightTrack(allocator) {
    AddAutomataPlugin(30,
                      NewCallbackPtr(this, &StraightTrack::SimulateAllRoutes));
  }

  virtual const GlobalVariable *LookupCloseDetector(
      const CtrlTrackInterface *from) {
    return nullptr;
  }

  // Returns the first occupancy detector outside of train length from the
  // interface `from'. Returns NULL if no such detector was found.
  virtual const GlobalVariable *LookupFarDetector(
      const CtrlTrackInterface *from) {
    return FindOtherSide(from)->binding()->LookupNextDetector();
  }
};

// A signal is a piece where trains coming from side_a towards side_b can (and
// shall) stop if the signal is red.
class SignalPiece : public StraightTrackShort {
 public:
  SignalPiece(const EventBlock::Allocator &allocator,
              GlobalVariable *request_green, GlobalVariable *signal)
      : StraightTrackShort(allocator),
        request_green_(request_green),
        signal_(signal) {
    // We "override" the occupancy detection by just copying the occupancy
    // value from the previous piece of track.
    RemoveAutomataPlugins(20);
    AddAutomataPlugin(20, NewCallbackPtr(this, &SignalPiece::SignalOccupancy));
    // Signals have custom route setting logic in the forward direction.
    RemoveAutomataPlugins(30);
    AddAutomataPlugin(30, NewCallbackPtr(this, &SignalPiece::SignalRoute));
  }

  virtual const GlobalVariable *LookupCloseDetector(
      const CtrlTrackInterface *from) {
    return FindOtherSide(from)->binding()->LookupCloseDetector();
  }

  // Returns the first occupancy detector outside of train length from the
  // interface `from'. Returns NULL if no such detector was found.
  virtual const GlobalVariable *LookupFarDetector(
      const CtrlTrackInterface *from) {
    return FindOtherSide(from)->binding()->LookupFarDetector();
  }

  void SignalRoute(Automata *aut);
  void SignalOccupancy(Automata *aut);

 private:
  GlobalVariable *request_green_;
  GlobalVariable *signal_;
};

class StandardBlock : public StraightTrackInterface {
 public:
  StandardBlock(Board *brd, PhysicalSignal *physical,
                EventBlock::Allocator *parent_alloc, const string &base_name,
                int num_to_allocate = 80)
      : alloc_(parent_alloc->Allocate(base_name, num_to_allocate, 8)),
        base_name_(base_name),
        request_green_(alloc_.Allocate("request_green")),
        body_(alloc_.Allocate("body", 24, 8)),
        body_det_(alloc_.Allocate("body_det", 24, 8),
                  physical->sensor_raw),
        signal_(alloc_.Allocate("signal", 24, 8),
                request_green_.get(), physical->signal_raw),
        physical_(physical),
        aut_body_(name() + ".body", brd, &body_),
        aut_body_det_(name() + ".body_det", brd, &body_det_),
        aut_signal_(name() + ".signal", brd, &signal_) {
    BindSequence({&body_, &body_det_, &signal_});
  }

 protected:
  EventBlock::Allocator alloc_;
  string base_name_;

 public:
  virtual CtrlTrackInterface *side_a() { return body_.side_a(); }
  virtual CtrlTrackInterface *side_b() { return signal_.side_b(); }

  GlobalVariable *request_green() { return request_green_.get(); }
  const GlobalVariable &route_in() const { return *body_det_.route_set_ab_; }
  const GlobalVariable &route_out() const { return *signal_.route_set_ab_; }
  const GlobalVariable &detector() const { return *body_det_.simulated_occupancy_; }

  PhysicalSignal *p() { return physical_; }

  /** Returns the basename of the block (not including path). */
  const string& base_name() {
    return base_name_;
  }

  /** Returns the full name of the block (including path). */
  const string& name() {
    return alloc_.name();
  }

 private:
  std::unique_ptr<GlobalVariable> request_green_;

 public:
  StraightTrackLong body_;
  StraightTrackWithRawDetector body_det_;
  SignalPiece signal_;

 private:
  PhysicalSignal *physical_;

  StandardPluginAutomata aut_body_;
  StandardPluginAutomata aut_body_det_;
  StandardPluginAutomata aut_signal_;

  DISALLOW_COPY_AND_ASSIGN(StandardBlock);
};

class StandardMiddleDetector : public StraightTrackWithRawDetector {
 public:
  StandardMiddleDetector(Board *brd, const GlobalVariable *sensor_raw,
                         const EventBlock::Allocator &alloc)
      : StraightTrackWithRawDetector(
            EventBlock::Allocator(&alloc, "body_det", 24, 8), sensor_raw),
        aut_(alloc.name() + ".det", brd, this) {}

 private:
  StandardPluginAutomata aut_;
};

class TurnoutInterface {
 public:
  virtual CtrlTrackInterface *side_points() = 0;
  virtual CtrlTrackInterface *side_closed() = 0;
  virtual CtrlTrackInterface *side_thrown() = 0;
};

void ClearAutomataVariables(Automata *aut);

class TurnoutBase : public TurnoutInterface,
                    private OccupancyLookupInterface,
                    public virtual AutomataPlugin {
 public:
  enum State { TURNOUT_CLOSED, TURNOUT_THROWN, TURNOUT_DONTCARE };

  TurnoutBase(const EventBlock::Allocator &allocator)
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
        tmp_route_setting_in_progress_(
            allocator.Allocate("tmp_route_setting_in_progress")),
        detector_next_(allocator.Allocate("detector_next")),
        detector_far_(allocator.Allocate("detector_far")) {
    directions_.push_back(Direction(&side_points_, &side_closed_,
                                    route_set_PC_.get(),
                                    route_pending_PC_.get(), TURNOUT_CLOSED));
    directions_.push_back(Direction(&side_points_, &side_thrown_,
                                    route_set_PT_.get(),
                                    route_pending_PT_.get(), TURNOUT_THROWN));
    directions_.push_back(Direction(&side_closed_, &side_points_,
                                    route_set_CP_.get(),
                                    route_pending_CP_.get(), TURNOUT_DONTCARE));
    directions_.push_back(Direction(&side_thrown_, &side_points_,
                                    route_set_TP_.get(),
                                    route_pending_TP_.get(), TURNOUT_DONTCARE));
    AddAutomataPlugin(20, NewCallbackPtr(this, &TurnoutBase::TurnoutOccupancy));
    AddAutomataPlugin(25, NewCallbackPtr(this, &TurnoutBase::ProxyDetectors));
    AddAutomataPlugin(29, NewCallbackPtr(&ClearAutomataVariables));
    AddAutomataPlugin(30, NewCallbackPtr(this, &TurnoutBase::TurnoutRoute));
    AddAutomataPlugin(35,
                      NewCallbackPtr(this, &TurnoutBase::PopulateAnyRouteSet));
  }

  virtual CtrlTrackInterface *side_points() { return &side_points_; }
  virtual CtrlTrackInterface *side_closed() { return &side_closed_; }
  virtual CtrlTrackInterface *side_thrown() { return &side_thrown_; }

  const GlobalVariable *any_route() { return any_route_set_.get(); }

 protected:
  FRIEND_TEST(LogicTest, FixedTurnout);
  FRIEND_TEST(LogicTest, MovableTurnout);

  CtrlTrackInterface side_points_;
  CtrlTrackInterface side_closed_;
  CtrlTrackInterface side_thrown_;

  struct Direction {
    Direction(CtrlTrackInterface *f, CtrlTrackInterface *t, GlobalVariable *r,
              GlobalVariable *rp, State state_cond)
        : from(f),
          to(t),
          route(r),
          route_pending(rp),
          state_condition(state_cond) {}
    CtrlTrackInterface *from;
    CtrlTrackInterface *to;
    GlobalVariable *route;
    GlobalVariable *route_pending;
    State state_condition;
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

  // The "close detector" bit looking from the points.
  std::unique_ptr<GlobalVariable> detector_next_;
  // The "far detector" bit looking from the points.
  std::unique_ptr<GlobalVariable> detector_far_;

 private:
  virtual const GlobalVariable *LookupCloseDetector(
      const CtrlTrackInterface *from) {
    if (from == &side_points_) {
      return detector_next_.get();
    } else {
      return side_points_.binding()->LookupCloseDetector();
    }
  }

  virtual const GlobalVariable *LookupFarDetector(
      const CtrlTrackInterface *from) {
    if (from == &side_points_) {
      return detector_far_.get();
    } else {
      return side_points_.binding()->LookupFarDetector();
    }
  }

  void PopulateAnyRouteSet(Automata *aut);
  void ProxyDetectors(Automata *aut);
  void TurnoutOccupancy(Automata *aut);
  void TurnoutRoute(Automata *aut);
};

class MovableTurnout : public TurnoutBase {
 public:
  MovableTurnout(const EventBlock::Allocator &allocator, MagnetDef *def)
      : TurnoutBase(allocator), magnet_(def) {
    AddAutomataPlugin(110, NewCallbackPtr(this, &MovableTurnout::CopyState));
  }

  MagnetDef *magnet() { return magnet_; }

 private:
  // Copies the turnout state from the physical turnout's actual state.
  void CopyState(Automata *aut);

  MagnetDef *magnet_;
};

class FixedTurnout : public TurnoutBase {
 public:
  FixedTurnout(State state, const EventBlock::Allocator &allocator)
      : TurnoutBase(allocator), state_(state) {
    AddAutomataPlugin(9, NewCallbackPtr(this, &FixedTurnout::FixTurnoutState));

    directions_.clear();
    directions_.push_back(Direction(&side_closed_, &side_points_,
                                    route_set_CP_.get(),
                                    route_pending_CP_.get(), TURNOUT_DONTCARE));
    directions_.push_back(Direction(&side_thrown_, &side_points_,
                                    route_set_TP_.get(),
                                    route_pending_TP_.get(), TURNOUT_DONTCARE));
    switch (state) {
      case TURNOUT_CLOSED:
        directions_.push_back(
            Direction(&side_points_, &side_closed_, route_set_PC_.get(),
                      route_pending_PC_.get(), TURNOUT_CLOSED));
        break;
      case TURNOUT_THROWN:
        directions_.push_back(
            Direction(&side_points_, &side_thrown_, route_set_PT_.get(),
                      route_pending_PT_.get(), TURNOUT_THROWN));
        break;
      case TURNOUT_DONTCARE:
        assert(false);
        break;
    }
  }

 private:
  void FixTurnoutState(Automata *aut);

  State state_;
};

class StandardMovableTurnout {
 public:
  StandardMovableTurnout(Board *brd, const EventBlock::Allocator &alloc,
                         MagnetDef *magnet)
      : b(alloc, magnet), aut_(alloc.name(), brd, &b) {}

  GlobalVariable *command() { return b.magnet()->command.get(); }
  const GlobalVariable &state() { return *b.magnet()->current_state; }

  MovableTurnout b;

 private:
  StandardPluginAutomata aut_;
};

class StandardFixedTurnout {
 public:
  StandardFixedTurnout(Board *brd, const EventBlock::Allocator &alloc,
                       FixedTurnout::State state)
      : b(state, alloc), aut_(alloc.name(), brd, &b) {}

  FixedTurnout b;

 private:
  StandardPluginAutomata aut_;
};

// This is the interface between train automatas and flip-flop automatas that
// grant permission to proceed to alternating trains.
class RequestClientInterface {
 public:
  RequestClientInterface(const string &name,
                         EventBlock::Allocator &parent_alloc)
      : name_(name),
        alloc_(parent_alloc.Allocate("client." + name, 3, 4)),
        request_(alloc_.Allocate("request")),
        granted_(alloc_.Allocate("granted")),
        taken_(alloc_.Allocate("taken")) {}

  // This bit will be set by the client (the train automata) if the train
  // routing is at the present location, and all other necessary conditions are
  // fulfilled for making the train progress. Other conditions include the
  // destination track being free, the intermediate blocks leading to the
  // destination track being all available, as well as any timer blocking the
  // train from proceeding being also fulfilled, etc.
  //
  // This bit will be cleared by the client when either the conditions do not
  // hold anymore, or when the granted permission is taken.
  GlobalVariable* request() { return request_.get(); }

  // This bit will be set by the flip-flop server automata. It will be cleared
  // by the server automata if it revokes the grant, or by the client automata
  // when it sets the taken bit.
  GlobalVariable* granted() { return granted_.get(); }

  // This bit will be set by the client when it sees that the permission is
  // granted and it removes the request bit. It will be cleared by the flipflop
  // automata together with the granted bit.
  GlobalVariable* taken() { return taken_.get(); }

 private:
  DISALLOW_COPY_AND_ASSIGN(RequestClientInterface);

  string name_;
  EventBlock::Allocator alloc_;
  std::unique_ptr<GlobalVariable> request_;
  std::unique_ptr<GlobalVariable> granted_;
  std::unique_ptr<GlobalVariable> taken_;
};

class FlipFlopClient;

class FlipFlopAutomata : public AutomataPlugin {
 public:
  FlipFlopAutomata(Board *brd, const string &name,
                   EventBlock::Allocator &parent_alloc)
      : name_(name),
        alloc_(parent_alloc.Allocate(name, 16, 8)),
        aut_(name, brd, this),
        aut(&aut_) {
    AddAutomataPlugin(100,
                      NewCallbackPtr(this, &FlipFlopAutomata::FlipFlopLogic));
  }

  EventBlock::Allocator& AddClient(FlipFlopClient* client);

  StateRef GetNewClientState() {
    return aut_.NewUserState();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(FlipFlopAutomata);

  void FlipFlopLogic(Automata* aut);

  string name_;
  EventBlock::Allocator alloc_;
  StandardPluginAutomata aut_;
  Automata* aut;

  // Members are externally owned.
  vector<FlipFlopClient*> clients_;
};

struct FlipFlopClient : public RequestClientInterface {
 public:
  FlipFlopClient(const string& name, FlipFlopAutomata* parent)
      : RequestClientInterface(name, parent->AddClient(this)),
        client_state(parent->GetNewClientState()) {}
  StateRef client_state;
 private:
  DISALLOW_COPY_AND_ASSIGN(FlipFlopClient);
};

static constexpr StateRef StWaiting(2);
static constexpr StateRef StReadyToGo(3);
static constexpr StateRef StRequestGreen(4);
static constexpr StateRef StGreenWait(5);
static constexpr StateRef StStartTrain(6);
static constexpr StateRef StMoving(7);
static constexpr StateRef StStopTrain(8);

static constexpr StateRef StRequestTransition(9);
static constexpr StateRef StTransitionDone(10);

static constexpr StateRef StTurnout(11);
static constexpr StateRef StTestCondition(12);


class TrainSchedule : public virtual AutomataPlugin {
 public:
  TrainSchedule(const string &name, Board *brd, uint64_t train_node_id,
                EventBlock::Allocator &&perm_alloc,
                EventBlock::Allocator &&alloc)
      : train_node_id_(train_node_id),
        permanent_alloc_(std::forward<EventBlock::Allocator>(perm_alloc)),
        alloc_(std::forward<EventBlock::Allocator>(alloc)),
        aut_(name, brd, this),
        aut(&aut_),
        is_reversed_(permanent_alloc_.Allocate("is_reversed")),
        current_block_detector_(aut_.ReserveVariable()),
        current_block_request_green_(aut_.ReserveVariable()),
        current_block_route_out_(aut_.ReserveVariable()),
        current_block_permaloc_(aut_.ReserveVariable()),
        current_block_routingloc_(current_block_permaloc_),
        current_direction_(aut_.ReserveVariable()),
        next_block_permaloc_(aut_.ReserveVariable()),
        next_block_route_in_(aut_.ReserveVariable()),
        next_block_detector_(aut_.ReserveVariable()),
        magnet_command_(aut_.ReserveVariable()),
        magnet_state_(aut_.ReserveVariable()),
        magnet_locked_(aut_.ReserveVariable()),
        permit_request_(aut_.ReserveVariable()),
        permit_granted_(aut_.ReserveVariable()),
        permit_taken_(aut_.ReserveVariable()),
        magnets_ready_(alloc_.Allocate("magnets_ready")),
        outgoing_route_conditions_(
            NewCallbackPtr(this, &TrainSchedule::AddCurrentOutgoingConditions)),
        current_location_(nullptr) {
    AddAutomataPlugin(9900, NewCallbackPtr(this, &TrainSchedule::HandleInit));
    AddAutomataPlugin(
        99, NewCallbackPtr(this, &TrainSchedule::ClearCurrentLocation));
    AddAutomataPlugin(100, NewCallbackPtr(this, &TrainSchedule::RunTransition));
    // 200 already needs the imported local variables
    AddAutomataPlugin(200,
                      NewCallbackPtr(this, &TrainSchedule::HandleBaseStates));
    AddAutomataPlugin(210,
                      NewCallbackPtr(this, &TrainSchedule::SendTrainCommands));
  }

  // Performs the steps needed for the transition of train to the next location.
  virtual void RunTransition(Automata* aut) = 0;

  // ImportNextBlock -> sets localvariables based on the current state.
  // ImportLastBlock -> sets localvariables based on the current state.
  //

  GlobalVariable* TEST_GetPermalocBit(StandardBlock* source) {
    size_t s = location_map_.size();
    auto* r = AllocateOrGetLocationByBlock(source)->permaloc();
    HASSERT(s == location_map_.size());
    return r;
  }

 protected:
  struct ScheduleLocation {
    ScheduleLocation()
        : respect_direction_(false) {}

    GlobalVariable* permaloc() {
      return permaloc_bit.get();
    }

    GlobalVariable* routingloc() {
      // TODO(balazs.racz): switch this to the other bit.
      return permaloc_bit.get();
    }

    // This bit will be 1 if the train is right now physically in (or
    // approaching) this signal.
    std::unique_ptr<GlobalVariable> permaloc_bit;
    // Currently unused. Eventually this will be 1 if the automata is currently
    // setting the route from this block outwards.
    std::unique_ptr<GlobalVariable> routingloc_bit;

    // Specifies whether the direction bit should be taken into account as
    // well. If this is true, there are multiple outgoing possibilities from
    // this location, and each has its own direction bit. Automata transition
    // states will also have to match the current direction bit. If this is
    // false, the direction bit is not imported.
    bool respect_direction_;
  };
  // Makes an eager transfer from block source to block dest. This transfer
  // will not be gated on anything; as soon as the train shows up in source and
  // destination is free, the train will move into the destination
  // block. Condiiton, if specified, will have to evaluate to true in order to
  // give a green to the train from the current block to the next.
  void AddEagerBlockTransition(StandardBlock *source, StandardBlock *dest,
                               OpCallback *condition = nullptr);

  // Adds an eager block transition between each successive pair of blocks in
  // this list.
  void AddEagerBlockSequence(
      const std::initializer_list<StandardBlock *> &blocks) {
    auto ia = blocks.begin();
    if (ia == blocks.end()) return;
    auto ib = blocks.begin(); ++ib;
    while (ib != blocks.end()) {
      AddEagerBlockTransition(*ia, *ib);
      ++ia;
      ++ib;
    }
  }

  // Adds a block transition that, when the evaluated condition is true, will
  // communicate with a flipflop automata to request permission to
  // proceed. Only when the permission si granted, will the train proceed with
  // requesting green from the control point logic.
  void AddBlockTransitionOnPermit(StandardBlock *source, StandardBlock *dest,
                                  RequestClientInterface *client,
                                  OpCallback *condition = nullptr);

  // Stops the train at the destination block. This is a terminal state and
  // probably should be used for testing only.
  void StopTrainAt(StandardBlock* dest);

  // Before requesting green outbounds from this location, sets the particular
  // turnout to the desired state.
  //
  // @param magnet is the turnout to set.
  // @param desired_state if true, the turnout will be thrown, if false, will
  // be set to closed.
  void SwitchTurnout(MagnetDef* magnet, bool desired_state);

 private:
  // Handles state == StInit. Sets the train into a known state at startup,
  // reading the externally provided bits for figuring out where the train is.
  void HandleInit(Automata *aut);

  void ClearCurrentLocation(Automata* aut) {
    current_location_ = nullptr;
  }


  void SendTrainCommands(Automata *aut);
  void HandleBaseStates(Automata *aut);

  // This helper function will add all IfXxx conditions to a command that are
  // needed to restrict it to the currently set route. It will restrict on the
  // permaloc or routingloc, and the outgoing route bit if necessary.
  void AddCurrentOutgoingConditions(Automata::Op* op);

 protected:
  // Allocates the permaloc bit for the current block if it does not exist
  // yet. Maps the current_block_permaloc_ bit onto it.
  void MapCurrentBlockPermaloc(StandardBlock* source);

  // Allocates the location bits for a particular block if does not exist;
  // returns the location structure. Does NOT transfer ownership.
  //
  // @param p arbitrary key that references the location.
  // @param name will be used to mark the location bits for debugging.
  ScheduleLocation* AllocateOrGetLocation(const void* p, const string& name);

  // Allocates the location bits for a particular block if does not exist;
  // returns the location structure. Does NOT transfer ownership.
  ScheduleLocation* AllocateOrGetLocationByBlock(StandardBlock* block) {
    return AllocateOrGetLocation(&block->detector(), block->name());
  }

  // Allocates a new or returns an existing helper bit. id has to be different
  // for each different helper bit to be requested. name will be used in the
  // debugging name of the new bit.
  GlobalVariable* GetHelperBit(const void* id, const string& name);

  // The train that we are driving.
  uint64_t train_node_id_;

  // These bits should be used for non-reconstructible state.
  EventBlock::Allocator permanent_alloc_;
  // These bits should be used for transient state.
  EventBlock::Allocator alloc_;

  StandardPluginAutomata aut_;
  // Alias to the above object for using in Def().
  Automata* aut;

  // 1 if the train is currently in reverse mode (loco pushing at the end).
  std::unique_ptr<GlobalVariable> is_reversed_;
  Automata::LocalVariable current_block_detector_;
  Automata::LocalVariable current_block_request_green_;
  Automata::LocalVariable current_block_route_out_;
  // The location bit for the current block will be mapped here.
  Automata::LocalVariable current_block_permaloc_;
  // The routing location for the current state. This bit will be 1 if the head
  // of the route set is at the current location. Currently maps to permaloc,
  // because we do not support advance routing.
  Automata::LocalVariable& current_block_routingloc_;
  // If the current state transition is direction sensitive (that is,
  // current_location_->respect_direction_ == true), then this bit will be 1 if
  // we are at the chosen transition direction.
  Automata::LocalVariable current_direction_;

  // The location bit for the next block will be mapped here.
  Automata::LocalVariable next_block_permaloc_;
  Automata::LocalVariable next_block_route_in_;
  Automata::LocalVariable next_block_detector_;

  // These variables are used by magnet settings.
  Automata::LocalVariable magnet_command_;
  Automata::LocalVariable magnet_state_;
  Automata::LocalVariable magnet_locked_;

  // These variables are used by request-granted protocol.
  Automata::LocalVariable permit_request_;
  Automata::LocalVariable permit_granted_;
  Automata::LocalVariable permit_taken_;

  // Used by the StTurnout state to know if all turnouts have already been set.
  std::unique_ptr<GlobalVariable> magnets_ready_;

  // Callback that will add all necessary checks for outgoing route setting.
  std::unique_ptr<OpCallback> outgoing_route_conditions_;

 private:
  // This will be set temporarily as the processing goes down the body of
  // RunTransition. It always points to the location structure for the source
  // of the current transition.
  ScheduleLocation* current_location_;

  // This map goes from the detector bit of the current train location (aka
  // state) to the struct that holds the allocated train location bits.
  std::map<const void*, ScheduleLocation> location_map_;

  // These helper bits are used by turnout setting commands.
  std::map<const void*, std::unique_ptr<GlobalVariable> > helper_bits_;
};

}  // namespace automata

#endif  // _AUTOMATA_CONTROL_LOGIC_HXX_
