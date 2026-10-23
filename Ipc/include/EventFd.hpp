#ifndef EVENT_FD_HPP
#define EVENT_FD_HPP

namespace telemetry::ipc
{

// Use eventfds created without EFD_SEMAPHORE. Operations retry interrupted calls;
// blocking descriptors wait until the operation can complete.

// Increase the eventfd counter by one.
[[nodiscard]]
bool notifyEvent(int fd) noexcept;

// Read the eventfd counter and reset it to zero.
[[nodiscard]]
bool consumeEvent(int fd) noexcept;

} // namespace telemetry::ipc

#endif
