// Included inside lan.cpp's private namespace. Xbox pointers stay valid until
// completion, as required by the overlapped Winsock contract.
struct LanWsaBuffer {
  rex::be<uint32_t> length, address;
};
struct LanWsaOverlapped {
  rex::be<uint32_t> status, transferred, flags, reserved, event;
};
static_assert(sizeof(LanWsaBuffer) == 8);
static_assert(sizeof(LanWsaOverlapped) == 20);

struct LanWsaOperation {
  SOCKET socket = INVALID_SOCKET;
  uint32_t handle = 0;
  WSAOVERLAPPED native{};
  std::vector<WSABUF> buffers;
  sockaddr_in address{};
  int address_size = sizeof(address);
  DWORD bytes = 0, flags = 0;
  bool receive = false, connected = false;
  // Relay mode: sends are encoded into `bounce`; receives land in `bounce`
  // and are decoded into the guest's `scatter` buffers. `ready` marks a
  // receive already filled from recovered datagrams (no native I/O issued).
  bool relay = false, ready = false;
  size_t payload_size = 0;
  std::vector<uint8_t> bounce;
  std::vector<WSABUF> scatter;
  ppc_ptr_t<LanWsaOverlapped> guest;
  ppc_ptr_t<rex::system::XSOCKADDR_IN> source;
  mapped_u32 source_size, byte_output, flag_output;
  ~LanWsaOperation() {
    if (native.hEvent)
      WSACloseEvent(native.hEvent);
  }
};

class LanWsaQueue {
  std::mutex mutex_;
  std::vector<std::unique_ptr<LanWsaOperation>> operations_;
  // Cancelled operations whose native event the worker may still be waiting
  // on; the worker frees them after its wait returns.
  std::vector<std::unique_ptr<LanWsaOperation>> retired_;
  // Wakes the worker when the pending set changes or on shutdown.
  HANDLE wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  std::thread worker_;
  bool stopping_ = false;

  // Relay receive pacing per peer, logged every 5 s: a max gap far above the
  // average is network jitter rather than local frame pacing.
  struct Arrivals {
    std::chrono::steady_clock::time_point last{};
    unsigned packets = 0;
    double total_ms = 0, max_ms = 0;
  };
  std::map<uint32_t, Arrivals> arrivals_;
  std::chrono::steady_clock::time_point arrivals_logged_ =
      std::chrono::steady_clock::now();
  void NoteArrival(uint32_t peer) {
    const auto now = std::chrono::steady_clock::now();
    auto &a = arrivals_[peer];
    if (a.packets++) {
      const double gap =
          std::chrono::duration<double, std::milli>(now - a.last).count();
      a.total_ms += gap;
      a.max_ms = std::max(a.max_ms, gap);
    }
    a.last = now;
    if (now - arrivals_logged_ < std::chrono::seconds(5))
      return;
    arrivals_logged_ = now;
    for (auto &[ip, stats] : arrivals_) {
      if (stats.packets > 1)
        REXLOG_INFO("FM2 LAN rx {}: {} packets/5s, gap avg {:.1f} ms max "
                    "{:.1f} ms",
                    IpText(IpBytes(ip)), stats.packets,
                    stats.total_ms / (stats.packets - 1), stats.max_ms);
      stats.packets = 0;
      stats.total_ms = stats.max_ms = 0;
    }
  }

  static void Complete(LanWsaOperation &op, uint32_t error,
                       bool notify = true) {
    if (!error) {
      if (op.byte_output)
        *op.byte_output = op.bytes;
      if (op.flag_output)
        *op.flag_output = op.flags;
      if (op.receive && op.source) {
        *op.source = {};
        op.source->sin_family = op.address.sin_family;
        op.source->sin_port = ntohs(op.address.sin_port);
        op.source->sin_addr = ntohl(op.address.sin_addr.s_addr);
        *op.source_size = 16;
      }
      static std::atomic<unsigned> logged{0};
      if (logged.fetch_add(1) < 400)
        REXLOG_INFO("FM2 LAN WSA {} {} bytes {} {}:{}",
                    op.receive ? "received" : "sent", op.bytes,
                    op.receive ? "from" : "to",
                    IpText(IpBytes(ntohl(op.address.sin_addr.s_addr))),
                    ntohs(op.address.sin_port));
    }
    if (!op.guest)
      return;
    op.guest->transferred = error ? 0 : op.bytes;
    op.guest->flags = op.flags;
    std::atomic_thread_fence(std::memory_order_release);
    op.guest->status = error ? (0x80070000u | (error & 0xffffu)) : 0;
    static std::atomic<unsigned> completed_logs{0};
    if (completed_logs.fetch_add(1) < 8)
      REXLOG_INFO("FM2 LAN completion error={} bytes={} event={:08X}", error,
                  op.bytes, uint32_t(op.guest->event));
    if (notify && uint32_t(op.guest->event)) {
      auto event = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(
          uint32_t(op.guest->event) & ~1u);
      if (event)
        event->Set(0, false);
      else
        REXLOG_WARN("FM2 LAN completion event not found {:08X}",
                    uint32_t(op.guest->event));
    }
  }
  static int Issue(LanWsaOperation &op) {
    auto native = op.guest ? &op.native : nullptr;
    op.address_size = sizeof(op.address);
    if (op.connected && op.receive)
      return WSARecv(op.socket, op.buffers.data(), DWORD(op.buffers.size()),
                     &op.bytes, &op.flags, native, nullptr);
    if (op.connected)
      return WSASend(op.socket, op.buffers.data(), DWORD(op.buffers.size()),
                     &op.bytes, op.flags, native, nullptr);
    if (op.receive)
      return WSARecvFrom(op.socket, op.buffers.data(),
                         DWORD(op.buffers.size()), &op.bytes, &op.flags,
                         reinterpret_cast<sockaddr *>(&op.address),
                         &op.address_size, native, nullptr);
    return WSASendTo(op.socket, op.buffers.data(), DWORD(op.buffers.size()),
                     &op.bytes, op.flags,
                     reinterpret_cast<sockaddr *>(&op.address),
                     sizeof(op.address), native, nullptr);
  }
  // Converts a successful relay transfer to what the guest expects. Returns
  // false for datagrams that are not game traffic, which must be re-received.
  bool Finish(LanWsaOperation &op) {
    if (!op.relay) {
      if (op.receive && op.bytes)
        NoteArrival(ntohl(op.address.sin_addr.s_addr));
      return true;
    }
    if (!op.receive) {
      op.bytes = DWORD(op.payload_size);
      return true;
    }
    auto datagrams =
        RelayDecode(op.handle, op.bounce.data(), int(op.bytes), op.address);
    if (datagrams.empty())
      return false;
    Deliver(op, datagrams.front());
    RelayQueue(op.handle, datagrams, 1);
    return true;
  }
  // Copies one tunnelled datagram into the guest's receive buffers.
  void Deliver(LanWsaOperation &op, const RelayDatagram &datagram) {
    size_t offset = 0;
    for (auto &buffer : op.scatter) {
      const size_t n =
          std::min<size_t>(buffer.len, datagram.payload.size() - offset);
      std::memcpy(buffer.buf, datagram.payload.data() + offset, n);
      offset += n;
    }
    op.bytes = DWORD(offset);
    op.address = {};
    op.address.sin_family = AF_INET;
    op.address.sin_port = htons(datagram.port);
    std::memcpy(&op.address.sin_addr.s_addr, datagram.from.data(), 4);
    NoteArrival(IpValue(datagram.from));
  }
  // Re-receives after a dropped relay datagram. Returns 0 when the operation
  // is complete, WSA_IO_PENDING when it is queued again, or a WSA error.
  uint32_t Retry(LanWsaOperation &op) {
    for (;;) {
      if (op.native.hEvent)
        WSAResetEvent(op.native.hEvent);
      op.flags = 0;
      const auto error = Issue(op) == 0 ? 0u : uint32_t(WSAGetLastError());
      if (error)
        return error;
      if (Finish(op))
        return 0;
    }
  }
  void Poll() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
      for (auto it = operations_.begin(); it != operations_.end();) {
        auto &op = **it;
        if (op.ready) {
          Complete(op, 0);
          it = operations_.erase(it);
          continue;
        }
        bool ok = WSAGetOverlappedResult(op.socket, &op.native, &op.bytes,
                                         FALSE, &op.flags) != FALSE;
        auto error = ok ? 0u : uint32_t(WSAGetLastError());
        if (error == WSA_IO_INCOMPLETE) {
          ++it;
          continue;
        }
        if (!error && !Finish(op)) {
          error = Retry(op);
          if (error == WSA_IO_PENDING) {
            ++it;
            continue;
          }
        }
        Complete(op, error);
        it = operations_.erase(it);
      }
      // Wait on the native completion events directly: a timed sleep runs at
      // the 15.6 ms Windows timer granularity, which showed up in races as
      // stuttering remote cars.
      std::vector<HANDLE> events{wake_};
      for (auto &op : operations_)
        if (events.size() < MAXIMUM_WAIT_OBJECTS)
          events.push_back(op->native.hEvent);
      const DWORD timeout =
          events.size() - 1 < operations_.size() ? 1 : 1000;
      lock.unlock();
      WaitForMultipleObjects(DWORD(events.size()), events.data(), FALSE,
                             timeout);
      lock.lock();
      retired_.clear();
    }
  }

public:
  ~LanWsaQueue() { Shutdown(); }
  uint32_t Submit(std::unique_ptr<LanWsaOperation> op) {
    std::lock_guard lock(mutex_);
    if (stopping_)
      return WSA_OPERATION_ABORTED;
    for (auto &pending : operations_)
      if (op->guest &&
          pending->guest.host_address() == op->guest.host_address())
        return WSAEINVAL;
    if (operations_.size() >= 128)
      return WSAENOBUFS;
    if (op->guest) {
      op->native.hEvent = WSACreateEvent();
      if (op->native.hEvent == WSA_INVALID_EVENT)
        return WSAGetLastError();
      op->guest->status = 0x103; // NTSTATUS_PENDING, not WSA last-error 997.
      op->guest->transferred = 0;
      // The guest shares this completion event with its network work queue.
      // Resetting it here can erase a queued sender wake-up. The private native
      // OVERLAPPED event starts nonsignaled; the guest owns its event reset.
    }
    uint32_t error;
    RelayDatagram recovered;
    if (op->relay && op->receive && RelayTakePending(op->handle, recovered)) {
      // A datagram recovered from an earlier packet is served first.
      Deliver(*op, recovered);
      op->ready = true;
      error = op->guest ? WSA_IO_PENDING : 0;
    } else {
      const int result = Issue(*op);
      error = result == 0 ? 0u : uint32_t(WSAGetLastError());
      // Overlapped receives always complete through the worker, even when
      // data was already queued: FM2's DirectPlay layer mishandled a joiner's
      // second simultaneous peer connection when one of them completed inline.
      if (!error && op->guest && op->receive)
        error = WSA_IO_PENDING;
      else if (!error && !Finish(*op))
        error = Retry(*op);
    }
    if (error == WSA_IO_PENDING && op->guest) {
      // Pending callers may have placed these immediate-result outputs on
      // their stack. Publish delayed results only through OVERLAPPED/GetResult.
      op->byte_output = {};
      op->flag_output = {};
      operations_.push_back(std::move(op));
      if (!worker_.joinable())
        worker_ = std::thread([this] { Poll(); });
      SetEvent(wake_);
    } else
      Complete(*op, error, error == 0);
    static std::atomic<unsigned> submit_logs{0};
    if (submit_logs.fetch_add(1) < 8)
      REXLOG_INFO("FM2 LAN WSA submit result={}", error);
    return error;
  }
  void Cancel(uint32_t handle, bool all = false) {
    std::lock_guard lock(mutex_);
    for (auto it = operations_.begin(); it != operations_.end();) {
      auto &op = **it;
      if (!all && op.handle != handle) {
        ++it;
        continue;
      }
      if (!op.ready) {
        CancelIoEx(reinterpret_cast<HANDLE>(op.socket), &op.native);
        // Keep the native OVERLAPPED and its guest buffers alive until Windows
        // acknowledges cancellation, before the socket or guest memory is
        // freed.
        WSAGetOverlappedResult(op.socket, &op.native, &op.bytes, TRUE,
                               &op.flags);
      }
      Complete(op, WSA_OPERATION_ABORTED);
      retired_.push_back(std::move(*it));
      it = operations_.erase(it);
    }
    SetEvent(wake_);
  }
  void Shutdown() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    Cancel(0, true);
    if (worker_.joinable())
      worker_.join();
    std::lock_guard lock(mutex_);
    retired_.clear();
  }
  uint32_t Result(ppc_ptr_t<LanWsaOverlapped> guest, mapped_u32 bytes,
                  mapped_u32 flags) {
    std::lock_guard lock(mutex_);
    static std::atomic<unsigned> result_logs{0};
    if (result_logs.fetch_add(1) < 8)
      REXLOG_INFO("FM2 LAN WSA result status={} bytes={}",
                  uint32_t(guest->status), uint32_t(guest->transferred));
    if (uint32_t(guest->status) == 0x103)
      return WSA_IO_INCOMPLETE;
    if (uint32_t(guest->status))
      return (uint32_t(guest->status) & 0xffff0000u) == 0x80070000u
                 ? (uint32_t(guest->status) & 0xffffu)
                 : uint32_t(guest->status);
    static std::atomic<unsigned> success_logs{0};
    if (success_logs.fetch_add(1) < 8)
      REXLOG_INFO("FM2 LAN WSA completed result bytes={}",
                  uint32_t(guest->transferred));
    *bytes = uint32_t(guest->transferred);
    *flags = uint32_t(guest->flags);
    return 0;
  }
};
LanWsaQueue wsa_queue;

uint32_t WsaFailure(uint32_t error) {
  static std::atomic<unsigned> errors{0};
  if (error != WSA_IO_PENDING && error != WSA_IO_INCOMPLETE &&
      error != WSAEWOULDBLOCK && errors.fetch_add(1) < 64)
    REXLOG_WARN("FM2 LAN WSA error {}", error);
  WSASetLastError(int(error));
  SocketError();
  return uint32_t(-1);
}
std::unique_ptr<LanWsaOperation>
WsaBuffers(uint32_t handle, ppc_ptr_t<LanWsaBuffer> buffers, uint32_t count) {
  if (!buffers || !count || count > 16)
    return {};
  auto op = std::make_unique<LanWsaOperation>();
  op->socket = Native(handle);
  op->handle = handle;
  if (op->socket == INVALID_SOCKET)
    return {};
  uint64_t total = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t length = buffers[i].length, address = buffers[i].address;
    total += length;
    if (total > 65536 ||
        (length && (!address || address + length < address ||
                    !REX_KERNEL_MEMORY()->LookupHeap(address) ||
                    !REX_KERNEL_MEMORY()->LookupHeap(address + length - 1))))
      return {};
    op->buffers.push_back(
        {length, reinterpret_cast<char *>(
                     REX_KERNEL_MEMORY()->TranslateVirtual(address))});
  }
  return op;
}
void RelayPrepareSend(LanWsaOperation &op, const Ip &to, uint16_t port) {
  op.relay = true;
  op.connected = false;
  std::vector<uint8_t> payload;
  for (const auto &buffer : op.buffers)
    payload.insert(payload.end(), buffer.buf, buffer.buf + buffer.len);
  op.payload_size = payload.size();
  op.bounce = RelayEncode(op.handle, to, port, payload.data(), payload.size());
  op.buffers = {WSABUF{ULONG(op.bounce.size()),
                       reinterpret_cast<char *>(op.bounce.data())}};
  op.address = relay.server;
}
void RelayPrepareReceive(LanWsaOperation &op) {
  op.relay = true;
  op.connected = false;
  op.scatter = std::move(op.buffers);
  op.bounce.resize(kRelayDatagram);
  op.buffers = {WSABUF{ULONG(op.bounce.size()),
                       reinterpret_cast<char *>(op.bounce.data())}};
}
u32 WsaSendTo(u32, u32 handle, ppc_ptr_t<LanWsaBuffer> buffers, u32 count,
              mapped_u32 bytes, u32 flags,
              ppc_ptr_t<rex::system::XSOCKADDR_IN> to, u32 size,
              ppc_ptr_t<LanWsaOverlapped> overlapped, mapped_void completion) {
  static std::atomic<unsigned> argument_logs{0};
  if (argument_logs.fetch_add(1) < 8)
    REXLOG_INFO("FM2 LAN WSA arguments buffers={} bytes_ptr={} overlapped={} "
                "callback={}",
                uint32_t(count), bool(bytes), bool(overlapped),
                bool(completion));
  if (completion)
    return WsaFailure(WSAEOPNOTSUPP);
  if (!to || uint32_t(size) < 16 || to->sin_family != AF_INET ||
      (!bytes && !overlapped))
    return WsaFailure(WSAEINVAL);
  auto op = WsaBuffers(handle, buffers, count);
  if (!op)
    return WsaFailure(WSAEINVAL);
  if (RelayActive())
    RelayPrepareSend(*op, IpBytes(uint32_t(to->sin_addr)),
                     uint16_t(to->sin_port));
  else
    op->address = NativeAddress(*to, false);
  op->flags = flags;
  op->byte_output = bytes;
  op->guest = overlapped;
  auto error = wsa_queue.Submit(std::move(op));
  return error ? WsaFailure(error) : 0;
}
u32 WsaReceiveFrom(u32, u32 handle, ppc_ptr_t<LanWsaBuffer> buffers, u32 count,
                   mapped_u32 bytes, mapped_u32 flags,
                   ppc_ptr_t<rex::system::XSOCKADDR_IN> from, mapped_u32 size,
                   ppc_ptr_t<LanWsaOverlapped> overlapped,
                   mapped_void completion) {
  if (completion)
    return WsaFailure(WSAEOPNOTSUPP);
  if (!flags || (!bytes && !overlapped) ||
      (from && (!size || uint32_t(*size) < 16)))
    return WsaFailure(WSAEINVAL);
  auto op = WsaBuffers(handle, buffers, count);
  if (!op)
    return WsaFailure(WSAEINVAL);
  op->receive = true;
  if (RelayActive())
    RelayPrepareReceive(*op);
  op->guest = overlapped;
  op->source = from;
  op->source_size = size;
  op->byte_output = bytes;
  op->flag_output = flags;
  op->flags = *flags;
  auto error = wsa_queue.Submit(std::move(op));
  return error ? WsaFailure(error) : 0;
}
u32 WsaGetResult(u32, u32 handle, ppc_ptr_t<LanWsaOverlapped> overlapped,
                 mapped_u32 bytes, u32 wait, mapped_u32 flags) {
  if (!overlapped || !bytes || !flags || Native(handle) == INVALID_SOCKET) {
    WsaFailure(WSAEINVAL);
    return 0;
  }
  uint32_t error;
  do {
    error = wsa_queue.Result(overlapped, bytes, flags);
    if (error != WSA_IO_INCOMPLETE || !uint32_t(wait))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (true);
  if (error) {
    WsaFailure(error);
    return 0;
  }
  return 1;
}
u32 CloseSocket(u32, u32 handle) {
  auto object = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(handle);
  if (!object)
    return WsaFailure(WSAENOTSOCK);
  wsa_queue.Cancel(handle);
  RelayForget(handle);
  object->Close();
  object->ReleaseHandle();
  return 0;
}

u32 WsaSend(u32, u32 handle, ppc_ptr_t<LanWsaBuffer> buffers, u32 count,
            mapped_u32 bytes, u32 flags, ppc_ptr_t<LanWsaOverlapped> overlapped,
            mapped_void completion) {
  if (completion)
    return WsaFailure(WSAEOPNOTSUPP);
  if (!bytes && !overlapped)
    return WsaFailure(WSAEINVAL);
  auto op = WsaBuffers(handle, buffers, count);
  if (!op)
    return WsaFailure(WSAEINVAL);
  op->connected = true;
  op->byte_output = bytes;
  op->flags = flags;
  op->guest = overlapped;
  if (RelayActive()) {
    const auto bound = RelayLookup(handle);
    if (!bound || !bound->connected)
      return WsaFailure(WSAENOTCONN);
    RelayPrepareSend(*op, bound->peer, bound->peer_port);
  } else {
    int size = sizeof(op->address);
    getpeername(op->socket, reinterpret_cast<sockaddr *>(&op->address),
                &size);
  }
  auto error = wsa_queue.Submit(std::move(op));
  return error ? WsaFailure(error) : 0;
}
u32 WsaReceive(u32, u32 handle, ppc_ptr_t<LanWsaBuffer> buffers, u32 count,
               mapped_u32 bytes, mapped_u32 flags,
               ppc_ptr_t<LanWsaOverlapped> overlapped, mapped_void completion) {
  if (completion)
    return WsaFailure(WSAEOPNOTSUPP);
  if (!flags || (!bytes && !overlapped))
    return WsaFailure(WSAEINVAL);
  auto op = WsaBuffers(handle, buffers, count);
  if (!op)
    return WsaFailure(WSAEINVAL);
  op->connected = true;
  op->receive = true;
  op->guest = overlapped;
  op->byte_output = bytes;
  op->flag_output = flags;
  op->flags = *flags;
  if (RelayActive())
    RelayPrepareReceive(*op);
  else {
    int size = sizeof(op->address);
    getpeername(op->socket, reinterpret_cast<sockaddr *>(&op->address),
                &size);
  }
  auto error = wsa_queue.Submit(std::move(op));
  return error ? WsaFailure(error) : 0;
}
u32 SocketConnect(u32, u32 handle, ppc_ptr_t<rex::system::XSOCKADDR_IN> to,
                  u32 size) {
  if (!to || uint32_t(size) < 16 || to->sin_family != AF_INET)
    return WsaFailure(WSAEINVAL);
  SOCKET socket = Native(handle);
  if (socket == INVALID_SOCKET)
    return uint32_t(-1);
  if (RelayActive()) {
    // UDP "connect" only fixes the default peer; keep tunnelling via the relay.
    if (!RelayLookup(handle)) {
      const auto error = RelayBindSocket(handle, socket, 0);
      if (error)
        return WsaFailure(error);
    }
    RelayConnect(handle, IpBytes(uint32_t(to->sin_addr)),
                 uint16_t(to->sin_port));
    return 0;
  }
  auto address = NativeAddress(*to, false);
  sockaddr_in current{};
  int length = sizeof(current);
  if (getsockname(socket, reinterpret_cast<sockaddr *>(&current), &length) ==
          0 &&
      !current.sin_port) {
    current.sin_family = AF_INET;
    auto ip = LocalIp();
    std::memcpy(&current.sin_addr.s_addr, ip.data(), 4);
    if (bind(socket, reinterpret_cast<sockaddr *>(&current), sizeof(current)) ==
        SOCKET_ERROR)
      return WsaFailure(WSAGetLastError());
  }
  int result =
      connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address));
  const auto error = result == SOCKET_ERROR ? uint32_t(WSAGetLastError()) : 0;
  REXLOG_INFO("FM2 LAN connected-socket target {}:{} result={}",
              IpText(IpBytes(ntohl(address.sin_addr.s_addr))),
              ntohs(address.sin_port), result);
  return error ? WsaFailure(error) : 0;
}

u32 SocketSend(u32, u32 handle, mapped_void data, u32 length, u32 flags) {
  if (!data)
    return WsaFailure(WSAEFAULT);
  SOCKET socket = Native(handle);
  if (socket == INVALID_SOCKET)
    return uint32_t(-1);
  int result;
  if (RelayActive()) {
    const auto bound = RelayLookup(handle);
    if (!bound || !bound->connected)
      return WsaFailure(WSAENOTCONN);
    result = RelaySendTo(handle, socket, data.host_address(), int(length),
                         bound->peer, bound->peer_port);
  } else
    result = send(socket, reinterpret_cast<const char *>(data.host_address()),
                  int(length), int(flags));
  auto error = result == SOCKET_ERROR ? uint32_t(WSAGetLastError()) : 0;
  static std::atomic<unsigned> count{0};
  if (count.fetch_add(1) < 12)
    REXLOG_INFO("FM2 LAN send result={} error={}", result, error);
  return error ? WsaFailure(error) : uint32_t(result);
}
u32 SocketReceive(u32, u32 handle, mapped_void data, u32 length, u32 flags) {
  if (!data)
    return WsaFailure(WSAEFAULT);
  SOCKET socket = Native(handle);
  if (socket == INVALID_SOCKET)
    return uint32_t(-1);
  int result;
  if (RelayActive()) {
    Ip source{};
    uint16_t port = 0;
    result = RelayReceiveFrom(handle, socket, data.host_address(),
                              int(length), int(flags), source, port);
  } else
    result = recv(socket, reinterpret_cast<char *>(data.host_address()),
                  int(length), int(flags));
  auto error = result == SOCKET_ERROR ? uint32_t(WSAGetLastError()) : 0;
  static std::atomic<unsigned> count{0};
  if (count.fetch_add(1) < 12)
    REXLOG_INFO("FM2 LAN recv result={} error={}", result, error);
  return error ? WsaFailure(error) : uint32_t(result);
}
