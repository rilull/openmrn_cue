/** \copyright
 * Copyright (c) 2013, Balazs Racz
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
 * \file main.cxx
 *
 * A simple application to demonstrate asynchronous interfaces.
 *
 * @author Balazs Racz
 * @date 7 Dec 2013
 */

#include <stdio.h>
#include <unistd.h>

#include "nmranet/SimpleStack.hxx"
#include "nmranet/SimpleNodeInfoMockUserFile.hxx"
#include "custom/AutomataControl.hxx"

static const nmranet::NodeID NODE_ID = 0x050101011403ULL;

nmranet::SimpleCanStack stack(NODE_ID);

nmranet::MockSNIPUserFile snip_user_file("Default user name",
                                         "Default user description");
const char *const nmranet::SNIP_DYNAMIC_FILENAME =
    nmranet::MockSNIPUserFile::snip_user_file_path;

int port = 12021;
const char *host = "localhost";
const char *device_path = nullptr;
const char *filename = "automata.bin";
uint64_t destination_nodeid = 0x050101011432ULL;
unsigned destination_alias = 0;
int space_id = 0xA0;
OVERRIDE_CONST(num_memory_spaces, 4);

void usage(const char *e) {
  fprintf(stderr,
          "Usage: %s ([-i destination_host] [-p port] | [-d device_path]) "
          "[(-n nodeid | -a alias)] [-w automata_write_file] [-s space_id]\n",
          e);
  fprintf(stderr,
          "Connects to an openlcb bus and reflashes a memory config block "
          "with the contents of a file.\n");
  fprintf(stderr,
          "The bus connection will be through an OpenLCB HUB on "
          "destination_host:port with OpenLCB over TCP "
          "(in GridConnect format) protocol, or through the CAN-USB device "
          "(also in GridConnect protocol) found at device_path. Device takes "
          "precedence over TCP host:port specification.");
  fprintf(stderr, "The default target is localhost:12021.\n");
  fprintf(stderr,
          "\nnodeid should be a 12-char hex string with 0x prefix "
          "and no separators, defaults '-b 0x050101011432'\n");
  fprintf(stderr,
          "\nalias should be a 3-char hex string with 0x prefix and "
          "no separators, like '-a 0x3F9'\n");
  fprintf(stderr,
          "\nautomata_write_file is a binary file that contains the data to "
          "write. Defaults to '-w automata.bin'\n");
  fprintf(
      stderr,
      "\nspace_id is the memory space to write to. Defaults to '-s 0xA0'\n");
  exit(1);
}

void parse_args(int argc, char *argv[]) {
  int opt;
  while ((opt = getopt(argc, argv, "hi:p:d:n:a:w:s:")) >= 0) {
    switch (opt) {
      case 'h':
        usage(argv[0]);
        break;
      case 'i':
        host = optarg;
        break;
      case 'p':
        port = atoi(optarg);
        break;
      case 'd':
        device_path = optarg;
        break;
      case 'n':
        destination_nodeid = strtoll(optarg, nullptr, 16);
        break;
      case 'a':
        destination_alias = strtoul(optarg, nullptr, 16);
        break;
      case 'w':
        filename = optarg;
        break;
      case 's':
        space_id = atoi(optarg);
        break;
      default:
        fprintf(stderr, "Unknown option %c\n", opt);
        usage(argv[0]);
    }
  }
  if (!filename || (!destination_nodeid && !destination_alias)) {
    usage(argv[0]);
  }
}

string read_file_to_string(const char *filename) {
  int fd = ::open(filename, O_RDONLY);
  ERRNOCHECK("open", fd);
  char buf[1024];
  string data;
  while (true) {
    int ret = ::read(fd, buf, sizeof(buf));
    ERRNOCHECK("read", ret);
    if (ret == 0) return data;
    data.append(buf, ret);
  }
}

void send_datagram(nmranet::DatagramPayload p) {
  SyncNotifiable n;
  BarrierNotifiable bn;

  nmranet::DatagramClient *client =
      stack.dg_service()->client_allocator()->next_blocking();

  nmranet::NodeHandle dst;
  dst.alias = destination_alias;
  dst.id = destination_nodeid;

  Buffer<nmranet::NMRAnetMessage> *b;
  mainBufferPool->alloc(&b);
  b->data()->reset(nmranet::Defs::MTI_DATAGRAM, stack.node()->node_id(), dst,
                   std::move(p));
  b->set_done(bn.reset(&n));

  client->write_datagram(b);
  n.wait_for_notification();

  if (client->result() != nmranet::DatagramClient::OPERATION_SUCCESS) {
    LOG(FATAL, "Datagram send result: %04x\n", client->result());
    exit(1);
  }

  stack.dg_service()->client_allocator()->typed_insert(client);
}

int appl_main(int argc, char *argv[]) {
  parse_args(argc, argv);

  string file_data = read_file_to_string(filename);
  LOG(INFO, "Read %d bytes from %s.", file_data.size(), filename);

  if (device_path) {
    stack.add_gridconnect_port(device_path);
  } else {
    stack.connect_tcp_gridconnect_hub(host, port);
  }
  stack.start_executor_thread("g_executor", 0, 0);
  while (!stack.node()->is_initialized()) usleep(1000);

  nmranet::DatagramPayload p;
  p.push_back(bracz_custom::AutomataDefs::DATAGRAM_CODE);
  p.push_back(bracz_custom::AutomataDefs::STOP_AUTOMATA);
  send_datagram(std::move(p));

  for (unsigned ofs = 0; ofs < file_data.size(); ofs += 64) {
    nmranet::DatagramPayload p =
        nmranet::MemoryConfigDefs::write_datagram(space_id, ofs);
    p.append(file_data.substr(ofs, 64));

    send_datagram(std::move(p));
  }

  p.clear();
  p.push_back(bracz_custom::AutomataDefs::DATAGRAM_CODE);
  p.push_back(bracz_custom::AutomataDefs::RESTART_AUTOMATA);
  send_datagram(std::move(p));

  return 0;
}
