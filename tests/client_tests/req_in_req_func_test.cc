/**
 * @file test_nested_rpc.cc
 * @brief Test issuing requests from within request handlers. This uses a
 * primary-backup setup, where the client sends requests to the primary,
 * which completes an RPC with *one* of the backups before replying.
 */
#include "client_tests.h"

// Set to true if the request handler or continuation at the primary or backup
// should run in the background.
bool primary_bg, backup_bg;

static constexpr uint8_t kTestDataByte = 10;
static constexpr size_t kTestNumReqs = 33;
static_assert(kTestNumReqs > kSessionReqWindow, "");

/// Request type used for client to primary
static constexpr uint8_t kTestReqTypeCP = kTestReqType + 1;

/// Request type used for primary to backup
static constexpr uint8_t kTestReqTypePB = kTestReqType + 2;

/// Per-request info maintained at the primary
class PrimaryReqInfo {
 public:
  size_t req_size_cp;        ///< Size of client-to-primary request
  ReqHandle *req_handle_cp;  ///< Handle for client-to-primary request
  MsgBuffer req_msgbuf_pb;   ///< MsgBuffer for primary-to-backup request
  MsgBuffer resp_msgbuf_pb;  ///< MsgBuffer for primary-to-backup response
  size_t etid;               ///< eRPC thread ID in the request handler

  PrimaryReqInfo(size_t req_size_cp, ReqHandle *req_handle_cp, size_t etid)
      : req_size_cp(req_size_cp), req_handle_cp(req_handle_cp), etid(etid) {}
};

union client_tag_t {
  struct {
    uint16_t req_i;
    uint16_t msgbuf_i;
    uint32_t req_size;
  } s;
  size_t _tag;

  client_tag_t(uint16_t req_i, uint16_t msgbuf_i, uint32_t req_size) {
    s.req_i = req_i;
    s.msgbuf_i = msgbuf_i;
    s.req_size = req_size;
  }

  client_tag_t(size_t _tag) : _tag(_tag) {}
};
static_assert(sizeof(client_tag_t) == sizeof(size_t), "");

/// Extended context for client
class AppContext : public BasicAppContext {
 public:
  FastRand fast_rand;
  MsgBuffer req_msgbuf[kSessionReqWindow];
  MsgBuffer resp_msgbuf[kSessionReqWindow];
  size_t num_reqs_sent = 0;
};

///
/// Server-side code
///

// Forward declaration
void primary_cont_func(RespHandle *, void *, size_t);

/// The primary's request handler for client-to-primary requests. Forwards the
/// received request to one of the backup servers.
void req_handler_cp(ReqHandle *req_handle_cp, void *_context) {
  auto *context = static_cast<BasicAppContext *>(_context);
  assert(!context->is_client);
  ASSERT_EQ(context->rpc->in_background(), primary_bg);

  // This will be freed by eRPC when the request handler returns
  const MsgBuffer *req_msgbuf_cp = req_handle_cp->get_req_msgbuf();
  size_t req_size_cp = req_msgbuf_cp->get_data_size();

  test_printf("Primary [Rpc %u]: Received request of length %zu.\n",
              context->rpc->get_rpc_id(), req_size_cp);

  // Record info for the request that we are now sending to the backup
  PrimaryReqInfo *srv_req_info =
      new PrimaryReqInfo(req_size_cp, req_handle_cp, context->rpc->get_etid());

  // Allocate request and response MsgBuffers for the request to the backup
  srv_req_info->req_msgbuf_pb = context->rpc->alloc_msg_buffer(req_size_cp);
  assert(srv_req_info->req_msgbuf_pb.buf != nullptr);

  srv_req_info->resp_msgbuf_pb = context->rpc->alloc_msg_buffer(req_size_cp);
  assert(srv_req_info->resp_msgbuf_pb.buf != nullptr);

  // Request to backup = client-to-server request + 1
  for (size_t i = 0; i < req_size_cp; i++) {
    srv_req_info->req_msgbuf_pb.buf[i] = req_msgbuf_cp->buf[i] + 1;
  }

  // Backup is server thread #1
  context->rpc->enqueue_request(
      context->session_num_arr[1], kTestReqTypePB, &srv_req_info->req_msgbuf_pb,
      &srv_req_info->resp_msgbuf_pb, primary_cont_func,
      reinterpret_cast<size_t>(srv_req_info));
}

/// The backups' request handler for primary-to-backup to requests. Echoes the
/// received request back to the primary.
void req_handler_pb(ReqHandle *req_handle, void *_context) {
  auto *context = static_cast<BasicAppContext *>(_context);
  assert(!context->is_client);
  ASSERT_EQ(context->rpc->in_background(), backup_bg);

  const MsgBuffer *req_msgbuf_pb = req_handle->get_req_msgbuf();
  size_t req_size = req_msgbuf_pb->get_data_size();

  test_printf("Backup [Rpc %u]: Received request of length %zu.\n",
              context->rpc->get_rpc_id(), req_size);

  // eRPC will free dyn_resp_msgbuf
  req_handle->dyn_resp_msgbuf = context->rpc->alloc_msg_buffer(req_size);
  assert(req_handle->dyn_resp_msgbuf.buf != nullptr);

  // Response to primary = request + 1
  for (size_t i = 0; i < req_size; i++) {
    req_handle->dyn_resp_msgbuf.buf[i] = req_msgbuf_pb->buf[i] + 1;
  }

  req_handle->prealloc_used = false;
  context->rpc->enqueue_response(req_handle);
}

/// The primary's continuation function when it gets a response from a backup
void primary_cont_func(RespHandle *resp_handle_pb, void *_context,
                       size_t _tag) {
  auto *context = static_cast<BasicAppContext *>(_context);
  assert(!context->is_client);
  ASSERT_EQ(context->rpc->in_background(), primary_bg);

  const MsgBuffer *resp_msgbuf_pb = resp_handle_pb->get_resp_msgbuf();
  test_printf("Primary [Rpc %u]: Received response of length %zu.\n",
              context->rpc->get_rpc_id(), resp_msgbuf_pb->get_data_size());

  // Check that we're still running in the same thread as for the
  // client-to-primary request
  auto *srv_req_info = reinterpret_cast<PrimaryReqInfo *>(_tag);
  assert(srv_req_info->etid == context->rpc->get_etid());

  // Extract the request info
  size_t req_size_cp = srv_req_info->req_size_cp;
  ReqHandle *req_handle_cp = srv_req_info->req_handle_cp;
  assert(resp_msgbuf_pb->get_data_size() == req_size_cp);

  // Check the response from server #1
  for (size_t i = 0; i < req_size_cp; i++) {
    assert(srv_req_info->req_msgbuf_pb.buf[i] + 1 == resp_msgbuf_pb->buf[i]);
  }

  // eRPC will free dyn_resp_msgbuf
  req_handle_cp->dyn_resp_msgbuf = context->rpc->alloc_msg_buffer(req_size_cp);
  assert(req_handle_cp->dyn_resp_msgbuf.buf != nullptr);

  // Response to client = server-to-server response + 1
  for (size_t i = 0; i < req_size_cp; i++) {
    req_handle_cp->dyn_resp_msgbuf.buf[i] = resp_msgbuf_pb->buf[i] + 1;
  }

  // Free resources of the server-to-server request
  context->rpc->free_msg_buffer(srv_req_info->req_msgbuf_pb);
  context->rpc->free_msg_buffer(srv_req_info->resp_msgbuf_pb);
  delete srv_req_info;

  // Release the server-server response
  context->rpc->release_response(resp_handle_pb);

  // Send response to the client
  req_handle_cp->prealloc_used = false;
  context->rpc->enqueue_response(req_handle_cp);
}

///
/// Client-side code
///
void client_cont_func(RespHandle *, void *, size_t);  // Forward declaration

/// Enqueue a request to server 0 using the request MsgBuffer index msgbuf_i
void client_request_helper(AppContext *context, size_t msgbuf_i) {
  assert(msgbuf_i < kSessionReqWindow);

  size_t req_size = get_rand_msg_size(&context->fast_rand,
                                      context->rpc->get_max_data_per_pkt(),
                                      context->rpc->get_max_msg_size());

  context->rpc->resize_msg_buffer(&context->req_msgbuf[msgbuf_i], req_size);

  // Fill in all the bytes of the request MsgBuffer with msgbuf_i
  for (size_t i = 0; i < req_size; i++) {
    context->req_msgbuf[msgbuf_i].buf[i] = kTestDataByte;
  }

  client_tag_t tag(static_cast<uint16_t>(context->num_reqs_sent),
                   static_cast<uint16_t>(msgbuf_i),
                   static_cast<uint32_t>(req_size));
  test_printf("Client [Rpc %u]: Sending request %zu of size %zu\n",
              context->rpc->get_rpc_id(), context->num_reqs_sent, req_size);

  context->rpc->enqueue_request(
      context->session_num_arr[0], kTestReqTypeCP,
      &context->req_msgbuf[msgbuf_i], &context->resp_msgbuf[msgbuf_i],
      client_cont_func, *reinterpret_cast<size_t *>(&tag));

  context->num_reqs_sent++;
}

void client_cont_func(RespHandle *resp_handle, void *_context, size_t _tag) {
  auto *context = static_cast<AppContext *>(_context);
  assert(context->is_client);

  const MsgBuffer *resp_msgbuf = resp_handle->get_resp_msgbuf();

  // Extract info from tag
  auto tag = static_cast<client_tag_t>(_tag);
  size_t req_size = tag.s.req_size;
  size_t msgbuf_i = tag.s.msgbuf_i;

  test_printf("Client [Rpc %u]: Received response for req %u, length = %zu.\n",
              context->rpc->get_rpc_id(), tag.s.req_i,
              resp_msgbuf->get_data_size());

  // Check the response
  ASSERT_EQ(resp_msgbuf->get_data_size(), req_size);
  for (size_t i = 0; i < req_size; i++) {
    ASSERT_EQ(resp_msgbuf->buf[i], kTestDataByte + 3);
  }

  context->num_rpc_resps++;
  context->rpc->release_response(resp_handle);

  if (context->num_reqs_sent < kTestNumReqs) {
    client_request_helper(context, msgbuf_i);
  }
}

void client_thread(Nexus *nexus, size_t num_sessions) {
  // Create the Rpc and connect the sessions
  AppContext context;
  client_connect_sessions(nexus, context, num_sessions, basic_sm_handler);

  Rpc<CTransport> *rpc = context.rpc;

  // Start by filling the request window
  for (size_t i = 0; i < kSessionReqWindow; i++) {
    context.req_msgbuf[i] = rpc->alloc_msg_buffer(Rpc<CTransport>::kMaxMsgSize);
    assert(context.req_msgbuf[i].buf != nullptr);

    context.resp_msgbuf[i] =
        rpc->alloc_msg_buffer(Rpc<CTransport>::kMaxMsgSize);
    assert(context.resp_msgbuf[i].buf != nullptr);

    client_request_helper(&context, i);
  }

  wait_for_rpc_resps_or_timeout(context, kTestNumReqs, nexus->freq_ghz);
  assert(context.num_rpc_resps == kTestNumReqs);

  for (size_t i = 0; i < kSessionReqWindow; i++) {
    rpc->free_msg_buffer(context.req_msgbuf[i]);
  }

  // Disconnect the sessions
  context.num_sm_resps = 0;
  for (size_t i = 0; i < num_sessions; i++) {
    rpc->destroy_session(context.session_num_arr[i]);
  }
  wait_for_sm_resps_or_timeout(context, num_sessions, nexus->freq_ghz);
  assert(rpc->num_active_sessions() == 0);

  // Free resources
  delete rpc;
  client_done = true;
}

/// 1 primary, 1 backup, both in foreground
TEST(Base, BothInForeground) {
  primary_bg = false;
  backup_bg = false;

  auto reg_info_vec = {
      ReqFuncRegInfo(kTestReqTypeCP, req_handler_cp, ReqFuncType::kForeground),
      ReqFuncRegInfo(kTestReqTypePB, req_handler_pb, ReqFuncType::kForeground)};

  // 2 client sessions (=> 2 server threads), 0 background threads
  launch_server_client_threads(2, 0, client_thread, reg_info_vec,
                               ConnectServers::kTrue, 0.0);
}

/// 1 primary, 1 backup, primary in background
TEST(Base, PrimaryInBackground) {
  primary_bg = true;
  backup_bg = false;

  auto reg_info_vec = {
      ReqFuncRegInfo(kTestReqTypeCP, req_handler_cp, ReqFuncType::kBackground),
      ReqFuncRegInfo(kTestReqTypePB, req_handler_pb, ReqFuncType::kForeground)};

  // 2 client sessions (=> 2 server threads), 3 background threads
  launch_server_client_threads(2, 1, client_thread, reg_info_vec,
                               ConnectServers::kTrue, 0.0);
}

/// 1 primary, 1 backup, both in background
TEST(Base, BothInBackground) {
  primary_bg = true;
  backup_bg = true;

  auto reg_info_vec = {
      ReqFuncRegInfo(kTestReqTypeCP, req_handler_cp, ReqFuncType::kBackground),
      ReqFuncRegInfo(kTestReqTypePB, req_handler_pb, ReqFuncType::kBackground)};

  // 2 client sessions (=> 2 server threads), 3 background threads
  launch_server_client_threads(2, 3, client_thread, reg_info_vec,
                               ConnectServers::kTrue, 0.0);
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
