/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_LB_FORWARD_HTTP_REQUEST_HXX
#define BENG_LB_FORWARD_HTTP_REQUEST_HXX

struct LbHttpConnection;
struct HttpServerRequest;
struct LbClusterConfig;
class CancellablePointer;

void
ForwardHttpRequest(LbHttpConnection &connection,
                   HttpServerRequest &request,
                   const LbClusterConfig &cluster_config,
                   CancellablePointer &cancel_ptr);

#endif