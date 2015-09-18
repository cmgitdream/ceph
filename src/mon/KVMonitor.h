// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2015 Min Chen <minchen87@outlook.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

/*
 * The KVMonitor is used to save key/value pair in monitor cluster.
 */

#ifndef CEPH_KVMONITOR_H
#define CEPH_KVMONITOR_H

#include <map>
#include <set>

using namespace std;

#include "include/types.h"
#include "msg/Messenger.h"

#include "PaxosService.h"
#include "MonitorDBStore.h"
#include "include/encoding.h"

class MMonCommand;

class KVMonitor : public PaxosService {
public:
  class KVMap {
  public:
    version_t version;
    map<string, string> kvs;

    KVMap()
      : version(0), kvs()
    {}
    void encode(bufferlist &bl)
    {
      ::encode(version, bl);
      ::encode(kvs, bl);
    }
    void decode(bufferlist::iterator &p)
    {
      ::decode(version, p);
      ::decode(kvs, p);
    }
    void decode(bufferlist& bl)
    {
      bufferlist::iterator p = bl.begin();
      decode(p);
    }
  };

  KVMonitor(Monitor *mn, Paxos *p, const string& service_name)
    : PaxosService(mn, p, service_name),
    pending_kvmap(), need_update(false)
  {}

  ~KVMonitor() {}

  //the pending key/value pair to written to monitor cluster
  class KVMap pending_kvmap;

  void create_initial();

  void update_from_paxos(bool *need_bootstrap);

  void create_pending();

  void encode_pending(MonitorDBStore::TransactionRef t);
  virtual void encode_full(MonitorDBStore::TransactionRef t) {}

  bool preprocess_query(MonOpRequestRef op);
  bool prepare_update(MonOpRequestRef op);

  bool preprocess_command(MonOpRequestRef op);
  bool prepare_command(MonOpRequestRef op);

  bool should_propose(double& delay);

  void tick();

 private:
  bool need_update;
};

#endif
