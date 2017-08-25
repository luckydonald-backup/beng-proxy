/*
 * Copyright 2007-2017 Content Management AG
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Wrapper for the tcp_stock class to support load balancing.
 */

#ifndef BENG_PROXY_TCP_BALANCER_HXX
#define BENG_PROXY_TCP_BALANCER_HXX

#include "StickyHash.hxx"

#include "util/Compiler.h"

struct pool;
struct Balancer;
struct AddressList;
class TcpStock;
class StockGetHandler;
struct StockItem;
class CancellablePointer;
class SocketAddress;

struct TcpBalancer;

/**
 * Creates a new TCP connection stock.
 *
 * @param tcp_stock the underlying tcp_stock object
 * @param balancer the load balancer object
 * @return the new TCP connections stock (this function cannot fail)
 */
TcpBalancer *
tcp_balancer_new(TcpStock &tcp_stock, Balancer &balancer);

void
tcp_balancer_free(TcpBalancer *tcp_balancer);

/**
 * @param session_sticky a portion of the session id that is used to
 * select the worker; 0 means disable stickiness
 * @param timeout the connect timeout for each attempt [seconds]
 */
void
tcp_balancer_get(TcpBalancer &tcp_balancer, struct pool &pool,
                 bool ip_transparent,
                 SocketAddress bind_address,
                 sticky_hash_t session_sticky,
                 const AddressList &address_list,
                 unsigned timeout,
                 StockGetHandler &handler,
                 CancellablePointer &cancel_ptr);

#endif
