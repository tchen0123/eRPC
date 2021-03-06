/**
 * @file requestvote.h
 * @brief Handlers for requestvote RPC
 */

#include "smr.h"

#ifndef REQUESTVOTE_H
#define REQUESTVOTE_H

// The appendentries request sent over eRPC
struct app_rv_req_t {
  int node_id;
  msg_requestvote_t msg_rv;
};

// The appendentries response sent over eRPC
struct app_rv_resp_t {
  msg_requestvote_response_t msg_rv_resp;
};

void requestvote_handler(erpc::ReqHandle *req_handle, void *_context) {
  auto *c = static_cast<AppContext *>(_context);
  const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
  assert(req_msgbuf->get_data_size() == sizeof(app_rv_req_t));

  auto *rv_req = reinterpret_cast<app_rv_req_t *>(req_msgbuf->buf);
  assert(node_id_to_name_map.count(rv_req->node_id) != 0);

  printf("smr: Received requestvote request from %s [%s].\n",
         node_id_to_name_map[rv_req->node_id].c_str(),
         erpc::get_formatted_time().c_str());

  // This does a linear search, which is OK for a small number of Raft servers
  raft_node_t *requester_node = raft_get_node(c->server.raft, rv_req->node_id);
  assert(requester_node != nullptr);

  erpc::MsgBuffer &resp_msgbuf = req_handle->pre_resp_msgbuf;
  c->rpc->resize_msg_buffer(&resp_msgbuf, sizeof(app_rv_resp_t));
  req_handle->prealloc_used = true;

  // rv_req->msg_rv is valid only for the duration of this handler, which is OK
  // as msg_requestvote_t does not contain any dynamically allocated members.
  int e = raft_recv_requestvote(
      c->server.raft, requester_node, &rv_req->msg_rv,
      &reinterpret_cast<app_rv_resp_t *>(resp_msgbuf.buf)->msg_rv_resp);
  erpc::rt_assert(e == 0);

  c->rpc->enqueue_response(req_handle);
}

void requestvote_cont(erpc::RespHandle *, void *, size_t);  // Fwd decl

// Raft callback for sending requestvote request
static int __raft_send_requestvote(raft_server_t *, void *, raft_node_t *node,
                                   msg_requestvote_t *msg_rv) {
  auto *conn = static_cast<connection_t *>(raft_node_get_udata(node));
  AppContext *c = conn->c;

  if (!c->rpc->is_connected(conn->session_num)) {
    printf("smr: Cannot send requestvote request (disconnected).\n");
    return 0;
  }

  printf("smr: Sending requestvote request to node %s [%s].\n",
         node_id_to_name_map[raft_node_get_id(node)].c_str(),
         erpc::get_formatted_time().c_str());

  auto *rrt = new raft_req_tag_t();
  rrt->req_msgbuf = c->rpc->alloc_msg_buffer(sizeof(app_rv_req_t));
  erpc::rt_assert(rrt->req_msgbuf.buf != nullptr);

  rrt->resp_msgbuf = c->rpc->alloc_msg_buffer(sizeof(app_rv_resp_t));
  erpc::rt_assert(rrt->resp_msgbuf.buf != nullptr);

  rrt->node = node;

  auto *rv_req = reinterpret_cast<app_rv_req_t *>(rrt->req_msgbuf.buf);
  rv_req->node_id = c->server.node_id;
  rv_req->msg_rv = *msg_rv;

  c->rpc->enqueue_request(conn->session_num,
                          static_cast<uint8_t>(ReqType::kRequestVote),
                          &rrt->req_msgbuf, &rrt->resp_msgbuf, requestvote_cont,
                          reinterpret_cast<size_t>(rrt));
  return 0;
}

void requestvote_cont(erpc::RespHandle *resp_handle, void *_context,
                      size_t tag) {
  auto *c = static_cast<AppContext *>(_context);
  auto *rrt = reinterpret_cast<raft_req_tag_t *>(tag);

  if (likely(rrt->resp_msgbuf.get_data_size() > 0)) {
    // The RPC was successful
    assert(rrt->resp_msgbuf.get_data_size() == sizeof(app_rv_resp_t));

    printf("smr: Received requestvote response from node %s [%s].\n",
           node_id_to_name_map[raft_node_get_id(rrt->node)].c_str(),
           erpc::get_formatted_time().c_str());

    int e = raft_recv_requestvote_response(
        c->server.raft, rrt->node,
        &reinterpret_cast<app_rv_resp_t *>(rrt->resp_msgbuf.buf)->msg_rv_resp);
    erpc::rt_assert(e == 0);  // XXX: Doc says: Shutdown if e != 0
  } else {
    // This is a continuation-with-failure
    printf("smr: Requestvote RPC to node %s failed to complete [%s].\n",
           node_id_to_name_map[raft_node_get_id(rrt->node)].c_str(),
           erpc::get_formatted_time().c_str());
  }

  c->rpc->free_msg_buffer(rrt->req_msgbuf);
  c->rpc->free_msg_buffer(rrt->resp_msgbuf);
  delete rrt;  // Free allocated memory, XXX: Remove when we use pool

  c->rpc->release_response(resp_handle);
}

#endif
