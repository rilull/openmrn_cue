/** \copyright
 * Copyright (c) 2016, Balazs Racz
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
 * \file FindTrainNode.hxx
 *
 * A class that helps finding a train on the network.
 *
 * @author Balazs Racz
 * @date 7 Feb 2016
 */

#ifndef _BRACZ_COMMANDSTATION_FINDTRAINNODE_HXX_
#define _BRACZ_COMMANDSTATION_FINDTRAINNODE_HXX_

#include "commandstation/TrainDb.hxx"
#include "commandstation/AllTrainNodes.hxx"
#include "commandstation/FindProtocolDefs.hxx"
#include "executor/StateFlow.hxx"
#include "nmranet/If.hxx"
#include "nmranet/IfCan.hxx"
#include "nmranet/Node.hxx"
#include "nmranet/EventHandlerTemplates.hxx"

namespace commandstation {

struct RemoteFindTrainNodeRequest {
  typedef std::function<void(nmranet::EventState state, nmranet::NodeID)> ResultFn;
  /** Sends and OpenLCB request for finding a train node with a given address
      or cab number.  DccMode should be set to the expected drive type (bits
      0..2) if the train node needs to be freshly allocated. Set the bit
      OLCBUSER if the expected drive type is not known or don't care. Set the
      DCC_LONG_ADDRESS bit if the inbound address should be interpreted as a
      long address (even if it is a small number). */
  void reset(int address, bool exact, DccMode type, ResultFn res = nullptr) {
    event = FindProtocolDefs::address_to_query(address, exact, type);
    nodeId = 0;
    resultCallback = std::move(res);
  }
  /** Constructor with arbitrary set find_protocol_flags. These come from
   * FindProtocolDefs. */
  void reset(int address, uint8_t find_protocol_flags, ResultFn res = nullptr) {
    event = FindProtocolDefs::address_to_query(address, false, (DccMode)0);
    event &= ~UINT64_C(0xFF);
    event |= find_protocol_flags;
    nodeId = 0;
    resultCallback = std::move(res);
  }
  /** Copy-Constructor. */
  void reset(const RemoteFindTrainNodeRequest& params, ResultFn res = nullptr) {
    event = params.event;
    nodeId = 0;
    resultCallback = std::move(res);
  }
  /** Requests all train nodes. */
  void reset(ResultFn res = nullptr) {
    event = nmranet::TractionDefs::IS_TRAIN_EVENT;
    nodeId = 0;
    resultCallback = std::move(res);
  }

  enum {
    // if the resultCode is set to this value at the request start, the request
    // will keep outstanding after the timeout expires and delivering results,
    // until the next request is sent to this flow.
    PERSISTENT_REQUEST = 0x00103080,
    // Kills the previous persistent request.
    CANCEL_REQUEST = 0x00103081,
  };

  /// Event to query for.
  uint64_t event;
  /// Response
  nmranet::NodeID nodeId;

  BarrierNotifiable done;
  int resultCode;
  /// If this function is non-empty, then multiple results are accepted, and
  /// for each returning result we call this function. In this case the done
  /// notifiable will only be called when the timeout is passed.
  std::function<void(nmranet::EventState state, nmranet::NodeID)>
      resultCallback;
};

class RemoteFindTrainNode
    : public StateFlow<Buffer<RemoteFindTrainNodeRequest>, QList<1>> {
 public:
  /// @param node is a local node from which queries can be sent out to the
  /// network. @param train_db is the allocated local train store.
  RemoteFindTrainNode(nmranet::Node* node)
      : StateFlow<Buffer<RemoteFindTrainNodeRequest>, QList<1>>(node->iface()),
        node_(node),
        nodeIdLookup_(static_cast<nmranet::IfCan*>(node_->iface())) {}

  Action entry() override {
    // If there was a persistent request, this will kill any more responses.
    requestId_++;
    replyHandler_.listen_for(0);
    persistentRequest_.reset();
    if (message()->data()->resultCode ==
        RemoteFindTrainNodeRequest::CANCEL_REQUEST) {
      // nothing else to do.
      return return_with_error(0);
    }
    if (message()->data()->resultCode ==
        RemoteFindTrainNodeRequest::PERSISTENT_REQUEST) {
      // New persistent request.
      persistentRequest_.reset(transfer_message());
    }
    return allocate_and_call(iface()->global_message_write_flow(),
                             STATE(send_find_query));
  }

  Action send_find_query() {
    LOG(VERBOSE, "Send find query");
    auto* b = get_allocation_result(iface()->global_message_write_flow());

    uint64_t event = message()->data()->event;
    replyHandler_.listen_for(event);
    remoteMatch_ = {0, 0};
    b->data()->reset(nmranet::Defs::MTI_PRODUCER_IDENTIFY, node_->node_id(),
                     nmranet::eventid_to_buffer(event));
    iface()->global_message_write_flow()->send(b);
    return sleep_and_call(&timer_, MSEC_TO_NSEC(200), STATE(reply_timeout));
  }

  /// Callback from the event handler object when a producer identified comes
  /// back on the bus.
  virtual void handle_reply(nmranet::NodeHandle src, nmranet::EventState state) {
    LOG(INFO, "Bus reply %04x%08x alias %03x", nmranet::node_high(src.id),
        nmranet::node_low(src.id), src.alias);
    if (input()->resultCallback) {
      if (src.id) {
        // Have Node ID
        input()->resultCallback(state, src.id);
      } else {
        // Need to look up node ID.
        auto* b = nodeIdLookup_.alloc();
        b->data()->reset(node_, src);
        b->ref();
        int request_id = requestId_;
        b->data()->done.reset(new TempNotifiable([this, request_id, state, b](){
              auto bd = get_buffer_deleter(b);
              if (requestId_ != request_id || !input()) {
                LOG(INFO, "LookupID response: dropped stale data");
                droppedResults_++;
                return; // outdated.
              }
              input()->resultCallback(state, b->data()->handle.id);
            }));
        nodeIdLookup_.send(b);
      }
    } else {
      // Prevents receiving more replies.
      replyHandler_.listen_for(0);
      remoteMatch_ = src;
      timer_.trigger();
    }
  }

  Action reply_timeout() {
    LOG(VERBOSE, "sleep end");
    // Prevents more wakeups.
    replyHandler_.listen_for(0);
    if (input()->resultCallback) {
      // Client wanted multiple results, and the time for waiting for results
      // is over
      if (!nodeIdLookup_.is_waiting()) {
        return sleep_and_call(&timer_, MSEC_TO_NSEC(50), STATE(reply_timeout));
      }
      return return_with_error(0);
    }
    if (!remoteMatch_.id && !remoteMatch_.alias) {
      LOG(VERBOSE, "Bus match not found, allocating...");
      // No match
      return return_with_error(nmranet::Defs::ERROR_OPENMRN_NOT_FOUND);
    }
    if (remoteMatch_.id) {
      return return_ok(remoteMatch_.id);
    }
    // Need to look up the node id from the alias.
    return invoke_subflow_and_wait(&nodeIdLookup_, STATE(node_id_lookup_done),
                                   node_, remoteMatch_);
  }

  Action node_id_lookup_done() {
    auto* b = full_allocation_result(&nodeIdLookup_);
    if (b->data()->handle.id != 0) {
      return return_ok(b->data()->handle.id);
    } else {
      LOG(INFO, "Failed to match found train alias %03x to node id, error %04x",
          b->data()->handle.alias, b->data()->resultCode);
      return return_with_error(nmranet::Defs::ERROR_DST_NOT_FOUND);
    }
  }

  class ReplyHandler : public nmranet::SimpleEventHandler {
   public:
    ReplyHandler(RemoteFindTrainNode* parent) : parent_(parent) {
      nmranet::EventRegistry::instance()->register_handler(
          EventRegistryEntry(this, FindProtocolDefs::TRAIN_FIND_BASE),
          FindProtocolDefs::TRAIN_FIND_MASK);
    }

    ~ReplyHandler() {
      nmranet::EventRegistry::instance()->unregister_handler(this);
    }

    void HandleIdentifyGlobal(const EventRegistryEntry& registry_entry,
                              EventReport* event,
                              BarrierNotifiable* done) override {
      AutoNotify an(done);

      nmranet::event_write_helper1.WriteAsync(
          event->dst_node, nmranet::Defs::MTI_CONSUMER_IDENTIFIED_RANGE,
          nmranet::WriteHelper::global(),
          nmranet::eventid_to_buffer(FindProtocolDefs::TRAIN_FIND_BASE),
          done->new_child());
    }

    void HandleProducerIdentified(const EventRegistryEntry& registry_entry,
                                  EventReport* event,
                                  BarrierNotifiable* done) override {
      AutoNotify an(done);
      LOG(VERBOSE, "Reply Handler");
      if (event->event == request_) {
        parent_->handle_reply(event->src_node, event->state);
      } else {
        LOG(INFO, "Dropped event reply input request %08x%08x reply %08x%08x",
            FAKELLP(request_), FAKELLP(event->event));
      }
    };

    void listen_for(nmranet::EventId request) {
      LOG(INFO, "listen for %08x%08x", FAKELLP(request));
      request_ = request;
    }

   private:
    nmranet::EventId request_{0};
    RemoteFindTrainNode* parent_;
  } replyHandler_{this};

  Action return_ok(nmranet::NodeID nodeId) {
    input()->nodeId = nodeId;
    return return_with_error(0);
  }

  Action return_with_error(int error) {
    if (error) {
      input()->nodeId = 0;
    }
    input()->resultCode = error;
    input()->done.notify();
    return release_and_exit();
  }

  RemoteFindTrainNodeRequest* input() {
    if (persistentRequest_) {
      return persistentRequest_->data();
    }
    return message()->data();
  }

  nmranet::If* iface() {
    // We know that the service pointer is the node's interface from the
    // constructor.
    return static_cast<nmranet::If*>(service());
  }

  StateFlowTimer timer_{this};
  /// an openlcb train that may have answered our search
  nmranet::NodeHandle remoteMatch_;
  nmranet::Node* node_;
  /// A monotonically increasing identifier to decide if we moved on from the
  /// last request yet.
  volatile uint16_t requestId_{0};
  volatile uint16_t droppedResults_{0};
  BufferPtr<RemoteFindTrainNodeRequest> persistentRequest_;
  nmranet::NodeIdLookupFlow nodeIdLookup_;
};

struct FindTrainNodeRequest {
  /** Requests finding a train node with a given address or cab number.
      DccMode should be set to the expected drive type (bits 0..2) if the
      train node needs to be freshly allocated. Set the bit OLCBUSER if the
      expected drive type is not known or don't care. Set the
      DCC_LONG_ADDRESS bit if the inbound address should be interpreted as a
      long address (even if it is a small number). */
  void reset(DccMode type, int address) {
    this->address = address;
    this->type = type;
  }

  DccMode type;
  int address;

  BarrierNotifiable done;

  int resultCode;
  nmranet::NodeID nodeId;
};

class FindTrainNode : public StateFlow<Buffer<FindTrainNodeRequest>, QList<1>> {
 public:
  /// @param node is a local node from which queries can be sent out to the
  /// network. @param train_db is the allocated local train store.
  FindTrainNode(nmranet::Node* node, TrainDb* train_db,
                AllTrainNodes* local_train_nodes)
      : StateFlow<Buffer<FindTrainNodeRequest>, QList<1>>(node->iface()),
        trainDb_(train_db),
      allTrainNodes_(local_train_nodes),
      remoteLookupFlow_(node) {}

 private:
  Action entry() override { return call_immediately(STATE(try_find_in_db)); }

  /** Looks through all nodes that exist in the train DB and tries to find one
   * with the given legacy address. */
  Action try_find_in_db() {
    for (unsigned train_id = 0; train_id < allTrainNodes_->size(); ++train_id) {
      auto e = allTrainNodes_->get_traindb_entry(train_id);
      if (!e.get() || input()->address != e->get_legacy_address()) {
        continue;
      }
      // TODO: check drive mode.
      return return_ok(allTrainNodes_->get_train_node_id(train_id));
    }
    return call_immediately(STATE(try_find_on_openlcb));
  }

  Action try_find_on_openlcb() {
    return invoke_subflow_and_wait(&remoteLookupFlow_, STATE(olcb_find_done),
                                   input()->address, false, OLCBUSER);
  }

  Action olcb_find_done() {
    auto* b = full_allocation_result(&remoteLookupFlow_);
    nmranet::NodeID id = b->data()->nodeId;
    b->unref();
    if (id) {
      return return_ok(id);
    }
    return call_immediately(STATE(allocate_new_train));
  }

  /** Asks the AllTrainNodes structure to allocate a new train node. */
  Action allocate_new_train() {
    LOG(VERBOSE, "Allocate train node");
    nmranet::NodeID n =
        allTrainNodes_->allocate_node(input()->type, input()->address);
    return return_ok(n);
  }

  Action return_ok(nmranet::NodeID nodeId) {
    input()->nodeId = nodeId;
    return return_with_error(0);
  }

  Action return_with_error(int error) {
    if (error) {
      input()->nodeId = 0;
    }
    input()->resultCode = error;
    return_buffer();
    return exit();
  }

  FindTrainNodeRequest* input() { return message()->data(); }

  nmranet::If* iface() {
    // We know that the service pointer is the node's interface from the
    // constructor.
    return static_cast<nmranet::If*>(service());
  }

  TrainDb* trainDb_;
  AllTrainNodes* allTrainNodes_;
  RemoteFindTrainNode remoteLookupFlow_;
};

}  // namespace commandstation

#endif  // _BRACZ_COMMANDSTATION_FINDTRAINNODE_HXX_
