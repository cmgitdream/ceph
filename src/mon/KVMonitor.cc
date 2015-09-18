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

#include "KVMonitor.h"
#include "Monitor.h"
#include "MonitorDBStore.h"

#include "messages/MMonCommand.h"

#include "common/Timer.h"
#include "common/ceph_argparse.h"
#include "common/errno.h"

#include <sstream>
#include "common/config.h"
#include "common/cmdparse.h"
#include "include/str_list.h"
#include "include/assert.h"

#define dout_subsys ceph_subsys_mon
#undef dout_prefix
#define dout_prefix _prefix(_dout, mon, get_last_committed())
static ostream& _prefix(std::ostream *_dout, Monitor *mon, version_t v) {
  return *_dout << "mon." << mon->name << "@" << mon->rank
		<< "(" << mon->get_state_name()
		<< ").kv v" << v << " ";
}

void KVMonitor::create_initial()
{
  dout(10) << "create_initial" << dendl;
  pending_kvmap.version = 1;
}

void KVMonitor::update_from_paxos(bool *need_bootstrap)
{
  version_t version = get_last_committed();
  dout(10) << __func__ << " version " << version << dendl;
}

void KVMonitor::create_pending()
{
  pending_kvmap.kvs.clear();
}

void KVMonitor::encode_pending(MonitorDBStore::TransactionRef t)
{
  dout(10) << "encode_pending current_version = " << get_last_committed()
	<< ", next_version = " << (get_last_committed() + 1) << dendl;

  std::map<string, string>::iterator itr = pending_kvmap.kvs.begin();
  if (itr == pending_kvmap.kvs.end())
    return;

  dout(10) << "encode_pending kvmap not empty, so write transaction" << dendl;

  version_t version = get_last_committed() + 1;
  put_last_committed(t, version);

  bufferlist valbl;
  string key = itr->first;
  valbl.append(itr->second);

  if (valbl.length() == 0) {
    dout(10) << __func__ << " rm kv, key = " << key << dendl;
    t->erase(get_service_name(), key);
  } else {
    dout(10) << __func__ << " set kv, key = " << key << " val = " << itr->second << dendl;
    put_value(t, key, valbl);
  }
}

bool KVMonitor::preprocess_query(MonOpRequestRef op)
{
  PaxosServiceMessage *m = static_cast<PaxosServiceMessage*>(op->get_req());
  dout(10) << __func__ << " " << *m << " type = " << m->get_type() << dendl;
  switch (m->get_type()) {
    // READs
  case MSG_MON_COMMAND:
    return preprocess_command(op);
  default:
    assert(0);
    return true;
  }
}

bool KVMonitor::preprocess_command(MonOpRequestRef op)
{
  MMonCommand *m = static_cast<MMonCommand*>(op->get_req());
  int r = -1;
  bufferlist rdata;
  stringstream ss;

  map<string, cmd_vartype> cmdmap;
  if (!cmdmap_from_json(m->cmd, &cmdmap, ss)) {
    string rs = ss.str();
    mon->reply_command(op, -EINVAL, rs, rdata, get_last_committed());
    return true;
  }

  string prefix;
  cmd_getval(g_ceph_context, cmdmap, "prefix", prefix);

  MonSession *session = m->get_session();
  if (!session) {
    mon->reply_command(op, -EACCES, "access denied", get_last_committed());
    return true;
  }

  dout(10) << __func__ << " prefix = \"" << prefix << "\""<< dendl;

  if (prefix == "kv get") {
    need_update = false;
    string key;
    cmd_getval(g_ceph_context, cmdmap, "key", key);

    r = get_value(key, rdata);
    dout(10) << "key = " << key << " r = " << r << dendl;
    if (r == -ENOENT) {
      ss << "there is no key/vaule for " << key;
    }
    string rs;
    getline(ss, rs);
    //dout(10) << __func__ << ": " << rs << dendl;
    mon->reply_command(op, r, rs, rdata, get_last_committed());
    return true;
  } else if (prefix == "kv set") {
    return false;
  } else if (prefix == "kv rm") {
    return false;
  }
}

bool KVMonitor::prepare_update(MonOpRequestRef op)
{
  PaxosServiceMessage *m = static_cast<PaxosServiceMessage*>(op->get_req());
  dout(10) << "prepare_update " << *m << " from " << m->get_orig_source_inst() << dendl;

  switch (m->get_type()) {
  case MSG_MON_COMMAND:
    return prepare_command(op);
  default:
    assert(0);
  }
  return false;
}

bool KVMonitor::prepare_command(MonOpRequestRef op)
{
  MMonCommand *m = static_cast<MMonCommand*>(op->get_req());
  stringstream ss;
  string rs;
  int err = -EINVAL;

  map<string, cmd_vartype> cmdmap;
  if (!cmdmap_from_json(m->cmd, &cmdmap, ss)) {
    string rs = ss.str();
    mon->reply_command(op, -EINVAL, rs, get_last_committed());
    return true;
  }

  string prefix;
  cmd_getval(g_ceph_context, cmdmap, "prefix", prefix);

  MonSession *session = m->get_session();
  if (!session) {
    mon->reply_command(op, -EACCES, "access denied", get_last_committed());
    return true;
  }

  string key;
  string val;
  bufferlist rdata;
  MonitorDBStore::TransactionRef t(new MonitorDBStore::Transaction);
  cmd_getval(g_ceph_context, cmdmap, "key", key);

  if (prefix == "kv set") {
    bufferlist inbl;
    need_update = true;
    cmd_getval(g_ceph_context, cmdmap, "val", val);
    inbl.append(val);
    pending_kvmap.kvs.clear();
    pending_kvmap.kvs.insert(std::make_pair(key, val));
    dout(10) << __func__ << " key = " << key << ", val = " << val << dendl;
    wait_for_finished_proposal(op, new Monitor::C_Command(mon, op, 0, rs,
					get_last_committed() + 1));
    return true;
  } else if (prefix == "kv rm") {
    pending_kvmap.kvs.clear();
    pending_kvmap.kvs.insert(std::make_pair(key, ""));
    rs= "removed "+key;
    err = get_value(key, rdata);
    if (err == -ENOENT) {
      ss << "there is no key/vaule for " << key;
      need_update = false;
      goto out;
    }
    need_update = true;
    dout(10) << __func__ << " " << prefix << " key = " << key << dendl;
    wait_for_finished_proposal(op, new Monitor::C_Command(mon, op, 0, rs,
					get_last_committed() + 1));
    return true;
  } else {
    ss << "unknown command " << prefix;
  }

out:
  getline(ss, rs);
  mon->reply_command(op, err, rs, get_last_committed());
  return false;
}

bool KVMonitor::should_propose(double& delay)
{
  delay = 0.0;
  return true;
}

void KVMonitor::tick()
{
  bool do_propose = (get_last_committed() == 0) || need_update;
  if (do_propose)
    propose_pending();
  need_update = false;
}
