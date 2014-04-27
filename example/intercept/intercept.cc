/** @file

  an example hello world plugin

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS 1
#endif

#include <ts/ts.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// intercept plugin
//
// This plugin primarily demonstrates the use of server interceptions to allow a
// plugin to act as an origin server. It also demonstrates how to use
// TSNetConnect to make a TCP connection to another server, and the how to use
// the TSVConn APIs to transfer data between virtual connections.
//
// This plugin intercepts all cache misses and proxies them to a separate server
// that is assumed to be running on localhost:60000. The plugin does no HTTP
// processing at all, it simply shuffles data until the client closes the
// request. The TSQA test test-server-intercept exercises this plugin. You can
// enable extensive logging with the "intercept" diagnostic tag.


#define PLUGIN "intercept"
#define PORT 60000

#define VDEBUG(fmt, ...) TSDebug(PLUGIN, fmt, ##__VA_ARGS__)

#if DEBUG
#define VERROR(fmt, ...) TSDebug(PLUGIN, fmt, ##__VA_ARGS__)
#else
#define VERROR(fmt, ...) TSError("[%s] %s: " fmt, PLUGIN, __FUNCTION__, ##__VA_ARGS__)
#endif

#define VIODEBUG(vio, fmt, ...) VDEBUG("vio=%p vio.cont=%p, vio.cont.data=%p, vio.vc=%p " fmt, \
    (vio), TSVIOContGet(vio), TSContDataGet(TSVIOContGet(vio)), TSVIOVConnGet(vio), ##__VA_ARGS__)

static TSCont TxnHook;
static TSCont InterceptHook;

static int InterceptInterceptionHook(TSCont contp, TSEvent event, void * edata);
static int InterceptTxnHook(TSCont contp, TSEvent event, void * edata);

// We are going to stream data between Traffic Server and an
// external server. This structure represents the state of a
// streaming I/O request. Is is directional (ie. either a read or
// a write). We need two of these for each TSVConn; one to push
// data into the TSVConn and one to pull data out.
struct InterceptIOChannel
{
  TSVIO             vio;
  TSIOBuffer        iobuf;
  TSIOBufferReader  reader;

  InterceptIOChannel() : vio(NULL), iobuf(NULL), reader(NULL) {
  }

  ~InterceptIOChannel() {
    if (this->reader) {
      TSIOBufferReaderFree(this->reader);
    }

    if (this->iobuf) {
      TSIOBufferDestroy(this->iobuf);
    }
  }

  void read(TSVConn vc, TSCont contp) {
    TSReleaseAssert(this->vio == NULL);
    TSReleaseAssert((this->iobuf = TSIOBufferCreate()));
    TSReleaseAssert((this->reader = TSIOBufferReaderAlloc(this->iobuf)));

    this->vio = TSVConnRead(vc, contp, this->iobuf, INT64_MAX);
  }

  void write(TSVConn vc, TSCont contp) {
    TSReleaseAssert(this->vio == NULL);
    TSReleaseAssert((this->iobuf = TSIOBufferCreate()));
    TSReleaseAssert((this->reader = TSIOBufferReaderAlloc(this->iobuf)));

    this->vio = TSVConnWrite(vc, contp, this->reader, INT64_MAX);
  }

};

// A simple encapsulation of the IO state of a TSVConn. We need the TSVConn itself, and the
// IO metadata for the read side and the write side.
struct InterceptIO
{
  TSVConn             vc;
  InterceptIOChannel  readio;
  InterceptIOChannel  writeio;

  void close() {
    if (this->vc) {
      TSVConnClose(this->vc);
    }
    this->vc = NULL;
    this->readio.vio = this->writeio.vio = NULL;
  }
};

// Interception proxy state block. From our perspective, Traffic
// Server is the client, and the origin server on whose behalf we
// are intercepting is the server. Hence the "client" and
// "server" nomenclature here.
struct InterceptState
{
  TSHttpTxn   txn;          // The transaction on whose behalf we are intercepting.

  InterceptIO client;       // Server intercept VC state.
  InterceptIO server;       // Intercept origin VC state.

  InterceptState() : txn(NULL) {
  }

  ~InterceptState() {
  }
};

// Return the InterceptIO control block that owns the given VC.
static InterceptIO *
InterceptGetThisSide(InterceptState * istate, TSVConn vc)
{
  return (istate->client.vc == vc) ? &istate->client : &istate->server;
}

// Return the InterceptIO control block that doesn't own the given VC.
static InterceptIO *
InterceptGetOtherSide(InterceptState * istate, TSVConn vc)
{
  return (istate->client.vc == vc) ? &istate->server : &istate->client;
}

// Evaluates to a human-readable name for a TSVConn in the
// intercept proxy state.
static const char *
InterceptProxySide(const InterceptState * istate, const InterceptIO * io)
{
  return (io == &istate->client) ? "<client>"
    : (io == &istate->server) ? "<server>"
    : "<unknown>";
}

static const char *
InterceptProxySideVC(const InterceptState * istate, TSVConn vc)
{
  return (istate->client.vc && vc == istate->client.vc) ? "<client>"
    : (istate->server.vc && vc == istate->server.vc) ? "<server>"
    : "<unknown>";
}

static bool
InterceptAttemptDestroy(InterceptState * istate, TSCont contp)
{
  if (istate->server.vc == NULL && istate->client.vc == NULL) {
    VDEBUG("destroying server intercept state istate=%p contp=%p", istate, contp);
    TSContDataSet(contp, NULL);  // Force a crash if we get additional events.
    TSContDestroy(contp);
    delete istate;
    return true;
  }

  return false;
}

union socket_type {
  struct sockaddr_storage storage;
  struct sockaddr         sa;
  struct sockaddr_in      sin;
  struct sockaddr_in6     sin6;
};

union argument_type {
  void *            ptr;
  intptr_t          ecode;
  TSVConn           vc;
  TSVIO             vio;
  TSHttpTxn         txn;
  InterceptState *  istate;

  argument_type(void * _p) : ptr(_p) {}
};

static TSCont
InterceptContCreate(TSEventFunc hook, TSMutex mutexp, void * data)
{
  TSCont contp;

  TSReleaseAssert((contp = TSContCreate(hook, mutexp)));
  TSContDataSet(contp, data);
  return contp;
}

static bool
InterceptShouldInterceptRequest(TSHttpTxn txn)
{
  // Normally, this function would inspect the request and
  // determine whether it should be intercepted. We might examine
  // the URL path, or some headers. For the sake of this example,
  // we will intercept everything that is not a cache hit.

  int status;
  TSReleaseAssert(TSHttpTxnCacheLookupStatusGet(txn, &status) == TS_SUCCESS);
  return status != TS_CACHE_LOOKUP_HIT_FRESH;
}

// This function is called in response to a READ_READY event. We
// should transfer any data we find from one side of the transfer
// to the other.
static int64_t
InterceptTransferData(InterceptIO * from, InterceptIO * to)
{
  TSIOBufferBlock   block;
  int64_t           consumed = 0;

  // Walk the list of buffer blocks in from the read VIO.
  for (block = TSIOBufferReaderStart(from->readio.reader); block; block = TSIOBufferBlockNext(block)) {
    int64_t remain = 0;
    const char * ptr;

    VDEBUG("attempting to transfer %" PRId64 " available bytes",  TSIOBufferBlockReadAvail(block, from->readio.reader));

    // Take the data from each buffer block, and write it into
    // the buffer of the write VIO.
    ptr = TSIOBufferBlockReadStart(block, from->readio.reader, &remain);
    while (ptr && remain) {
      int64_t nbytes;

      nbytes = TSIOBufferWrite(to->writeio.iobuf, ptr, remain);
      remain -= nbytes;
      ptr += nbytes;
      consumed += nbytes;
    }
  }

  VDEBUG("consumed %" PRId64 " bytes reading from vc=%p, writing to vc=%p", consumed, from->vc, to->vc);
  if (consumed) {
    TSIOBufferReaderConsume(from->readio.reader, consumed);
    // Note that we don't have to call TSIOBufferProduce here.
    // This is because data passed into TSIOBufferWrite is
    // automatically "produced".
  }

  return consumed;
}

// Handle events from TSHttpTxnServerIntercept. The intercept
// starts with TS_EVENT_NET_ACCEPT, and then continues with
// TSVConn events.
static int
InterceptInterceptionHook(TSCont contp, TSEvent event, void * edata)
{
  argument_type arg(edata);

  VDEBUG("contp=%p, event=%s (%d), edata=%p", contp, TSHttpEventNameLookup(event), event, arg.ptr);

  switch (event) {
  case TS_EVENT_NET_ACCEPT: {
    // Set up the server intercept. We have the original
    // TSHttpTxn from the continuation. We need to connect to the
    // real origin and get ready to shuffle data around.
    TSAction    action;
    char        buf[INET_ADDRSTRLEN];
    socket_type addr;
    argument_type     cdata(TSContDataGet(contp));
    InterceptState *  istate = new InterceptState();

    istate->txn = cdata.txn;
    istate->client.vc = arg.vc;

    // This event is delivered by the continuation that we
    // attached in InterceptTxnHook, so the continuation data is
    // the TSHttpTxn pointer.
    VDEBUG("allocated server intercept state istate=%p for txn=%p", istate, cdata.txn);

    // Set up a connection to our real origin, which will be
    // 127.0.0.1:$PORT.
    memset(&addr, 0, sizeof(addr));
    addr.sin.sin_family = AF_INET;
    addr.sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // XXX config option
    addr.sin.sin_port = htons(PORT);                    // XXX config option

    VDEBUG("binding client vc=%p to %s:%u",
        istate->client.vc, inet_ntop(AF_INET, &addr.sin.sin_addr, buf, sizeof(buf)), (unsigned)ntohs(addr.sin.sin_port));

    // Reset the continuation data to be our intercept state
    // block. We will need this so that we can access both of the
    // VCs at the same time. We need to do this before calling
    // TSNetConnect so that we can handle the failure case.
    TSContDataSet(contp, istate);

    action = TSNetConnect(contp, &addr.sa);
    if (TSActionDone(action)) {
      VDEBUG("origin connection was done inline");
    }

    // We should not do anything after the TSNetConnect call. The
    // NET_CONNECT events take care of everything and we don't
    // want to risk referencing any stale data here.

    return TS_EVENT_NONE;
  }
  
  case TS_EVENT_NET_ACCEPT_FAILED: {
    // TS_EVENT_NET_ACCEPT_FAILED will be delivered if the
    // transaction is cancelled before we start tunnelling
    // through the server intercept. One way that this can happen
    // is if the intercept is attached early, and then we server
    // the document out of cache.
    argument_type cdata(TSContDataGet(contp));

    // There's nothing to do here except nuke the continuation
    // that was allocated in InterceptTxnHook().
    VDEBUG("cancelling server intercept request for txn=%p", cdata.txn);

    TSContDestroy(contp);
    return TS_EVENT_NONE;
  }

  case TS_EVENT_NET_CONNECT: {
    // TSNetConnect is asynchronous, so all we know right now is
    // that the connect was scheduled. We need to kick off some
    // I/O in order to start proxying data between the Traffic
    // Server core and our intercepted origin server. The server
    // intercept will begin the process by sending a read
    // request.
    argument_type cdata(TSContDataGet(contp));

    VDEBUG("connected to intercepted origin server, binding server vc=%p to istate=%p", arg.vc, cdata.istate);

    TSReleaseAssert(cdata.istate->client.vc != NULL);
    cdata.istate->server.vc = arg.vc;

    // Start reading the request from the server intercept VC.
    cdata.istate->client.readio.read(cdata.istate->client.vc, contp);
    VIODEBUG(cdata.istate->client.readio.vio, "started %s read", InterceptProxySide(cdata.istate, &cdata.istate->client));

    // Start reading the response from the intercepted origin server VC.
    cdata.istate->server.readio.read(cdata.istate->server.vc, contp);
    VIODEBUG(cdata.istate->server.readio.vio, "started %s read", InterceptProxySide(cdata.istate, &cdata.istate->server));

    // Start writing the response to the server intercept VC.
    cdata.istate->client.writeio.write(cdata.istate->client.vc, contp);
    VIODEBUG(cdata.istate->client.writeio.vio, "started %s write", InterceptProxySide(cdata.istate, &cdata.istate->client));

    // Start writing the request to the intercepted origin server VC.
    cdata.istate->server.writeio.write(cdata.istate->server.vc, contp);
    VIODEBUG(cdata.istate->server.writeio.vio, "started %s write", InterceptProxySide(cdata.istate, &cdata.istate->server));

    return TS_EVENT_NONE;
  }

  case TS_EVENT_NET_CONNECT_FAILED: {
    // Connecting to the intercepted origin failed. We should
    // pass the error on up to the server intercept.
    argument_type cdata(TSContDataGet(contp));

    VDEBUG("origin connection for txn=%p failed with error code %ld", cdata.istate->txn, arg.ecode);

    TSReleaseAssert(cdata.istate->client.vc != NULL);
    TSReleaseAssert(cdata.istate->server.vc == NULL);

    // We failed to connect to the intercepted origin. Abort the
    // server intercept since we cannot handle it.
    TSVConnAbort(cdata.istate->client.vc, arg.ecode);

    delete cdata.istate;
    TSContDestroy(contp);

    return TS_EVENT_NONE;
  }

  case TS_EVENT_VCONN_READ_READY: {
    argument_type cdata = TSContDataGet(contp);
    TSVConn       vc    = TSVIOVConnGet(arg.vio);
    InterceptIO * from  = InterceptGetThisSide(cdata.istate, vc);
    InterceptIO * to    = InterceptGetOtherSide(cdata.istate, vc);;
    int64_t       nbytes;

    VIODEBUG(arg.vio, "ndone=%" PRId64 " ntodo=%" PRId64, TSVIONDoneGet(arg.vio), TSVIONTodoGet(arg.vio));
    VDEBUG("reading vio=%p vc=%p, istate=%p is bound to client vc=%p and server vc=%p",
        arg.vio, TSVIOVConnGet(arg.vio), cdata.istate, cdata.istate->client.vc, cdata.istate->server.vc);

    if (to->vc == NULL) {
      VDEBUG("closing %s vc=%p", InterceptProxySide(cdata.istate, from), from->vc);
      from->close();
    }

    if (from->vc == NULL) {
      VDEBUG("closing %s vc=%p", InterceptProxySide(cdata.istate, to), to->vc);
      to->close();
    }

    if (InterceptAttemptDestroy(cdata.istate, contp)) {
      return TS_EVENT_NONE;
    }

    VDEBUG("reading from %s (vc=%p), writing to %s (vc=%p)",
        InterceptProxySide(cdata.istate, from), from->vc, InterceptProxySide(cdata.istate, to), to->vc);

    nbytes = InterceptTransferData(from, to);

    // Reenable the read VIO to get more events.
    if (nbytes) {
      TSVIO writeio = to->writeio.vio;
      VIODEBUG(writeio, "WRITE VIO ndone=%" PRId64 " ntodo=%" PRId64, TSVIONDoneGet(writeio), TSVIONTodoGet(writeio));
      TSVIOReenable(from->readio.vio); // Re-enable the read side.
      TSVIOReenable(to->writeio.vio); // Reenable the write side.
    }

    return TS_EVENT_NONE;
  }

  case TS_EVENT_VCONN_WRITE_READY: {
    // WRITE_READY events happen all the time, when the TSVConn
    // buffer drains. There's no need to do anything with these
    // because we only fill the buffer when we have data to read.
    // The exception is where one side of the proxied connection
    // has been closed. Then we want to close the other side.
    argument_type cdata = TSContDataGet(contp);
    TSVConn       vc    = TSVIOVConnGet(arg.vio);
    InterceptIO * to    = InterceptGetThisSide(cdata.istate, vc);
    InterceptIO * from  = InterceptGetOtherSide(cdata.istate, vc);;

    // If the other side is closed, close this side too, but only if there
    // we have drained the write buffer.
    if (from->vc == NULL) {
      VDEBUG("closing %s vc=%p with %" PRId64 " bytes to left",
          InterceptProxySide(cdata.istate, to), to->vc, TSIOBufferReaderAvail(to->writeio.reader));
      if (TSIOBufferReaderAvail(to->writeio.reader) == 0) {
        to->close();
      }
    }

    InterceptAttemptDestroy(cdata.istate, contp);
    return TS_EVENT_NONE;
  }

  case TS_EVENT_ERROR:
  case TS_EVENT_VCONN_EOS: {
    // If we get an EOS on one side, we should just send and EOS
    // on the other side too. The server intercept will always
    // send us an EOS after Traffic Server has finished reading
    // the response. Once that happens, we are also finished with
    // the intercepted origin server. The same reasoning applies
    // to receiving EOS from the intercepted origin server, and
    // when handling errors.

    TSVConn       vc    = TSVIOVConnGet(arg.vio);
    argument_type cdata = TSContDataGet(contp);

    InterceptIO * from  = InterceptGetThisSide(cdata.istate, vc);
    InterceptIO * to    = InterceptGetOtherSide(cdata.istate, vc);;

    VIODEBUG(arg.vio, "received EOS or ERROR from %s side", InterceptProxySideVC(cdata.istate, vc));

    // Close the side that we received the EOS event from.
    if (from) {
      VDEBUG("%s writeio has %" PRId64 " bytes left",
          InterceptProxySide(cdata.istate, from), TSIOBufferReaderAvail(from->writeio.reader));
      from->close();
    }

    // Should we also close the other side? Well, that depends on whether the reader
    // has drained the data. If we close too early they will see a truncated read.
    if (to) {
      VDEBUG("%s writeio has %" PRId64 " bytes left",
          InterceptProxySide(cdata.istate, to), TSIOBufferReaderAvail(to->writeio.reader));
      if (TSIOBufferReaderAvail(to->writeio.reader) == 0) {
        to->close();
      }
    }

    InterceptAttemptDestroy(cdata.istate, contp);
    return event == TS_EVENT_ERROR ? TS_EVENT_ERROR : TS_EVENT_NONE;
  }

  case TS_EVENT_VCONN_READ_COMPLETE:
    // We read data forever, so we should never get a
    // READ_COMPLETE.
    VIODEBUG(arg.vio, "unexpected TS_EVENT_VCONN_READ_COMPLETE");
    return TS_EVENT_NONE;

  case TS_EVENT_VCONN_WRITE_COMPLETE:
    // We write data forever, so we should never get a
    // WRITE_COMPLETE.
    VIODEBUG(arg.vio, "unexpected TS_EVENT_VCONN_WRITE_COMPLETE");
    return TS_EVENT_NONE;

  case TS_EVENT_VCONN_INACTIVITY_TIMEOUT:
    VERROR("unexpected event %s (%d) edata=%p", TSHttpEventNameLookup(event), event, arg.ptr);
    return TS_EVENT_ERROR;

  default:
    VERROR("unexpected event %s (%d) edata=%p", TSHttpEventNameLookup(event), event, arg.ptr);
    return TS_EVENT_ERROR;
  }
}

// Handle events that occur on the TSHttpTxn.
static int
InterceptTxnHook(TSCont contp, TSEvent event, void * edata)
{
  argument_type arg(edata);

  VDEBUG("contp=%p, event=%s (%d), edata=%p", contp, TSHttpEventNameLookup(event), event, arg.ptr);

  switch (event) {
    case TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE: {
    if (InterceptShouldInterceptRequest(arg.txn)) {
      TSCont c = InterceptContCreate(InterceptInterceptionHook, TSMutexCreate(), arg.txn);

      VDEBUG("intercepting orgin server request for txn=%p, cont=%p", arg.txn, c);
      TSHttpTxnServerIntercept(c, arg.txn);
    }

    break;
  }

  default:
    VERROR("unexpected event %s (%d)", TSHttpEventNameLookup(event), event);
    break;
  }

  TSHttpTxnReenable(arg.txn, TS_EVENT_HTTP_CONTINUE);
  return TS_EVENT_NONE;
}

void
TSPluginInit(int /* argc */, const char * /* argv */ [])
{
  TSPluginRegistrationInfo info;

  info.plugin_name = (char *)PLUGIN;
  info.vendor_name = (char *)"MyCompany";
  info.support_email = (char *)"ts-api-support@MyCompany.com";

  if (TSPluginRegister(TS_SDK_VERSION_3_0, &info) != TS_SUCCESS) {
    VERROR("plugin registration failed\n");
  }

  // XXX accept hostname and port arguments

  TxnHook = InterceptContCreate(InterceptTxnHook, NULL, NULL);
  InterceptHook = InterceptContCreate(InterceptInterceptionHook, NULL, NULL);

  // Wait until after the cache lookup to decide whether to
  // intercept a request. For cache hits we will never intercept.
  TSHttpHookAdd(TS_HTTP_CACHE_LOOKUP_COMPLETE_HOOK, TxnHook);
}