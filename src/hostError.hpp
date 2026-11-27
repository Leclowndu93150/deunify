#pragma once

#include <cstdint>

namespace deunify::detail {

/// Records the Win32 error code or HRESULT of a Status::HostFailure (or a refused commit) on
/// this thread. Every component calls it where it returns such a status.
void setLastHostError(uint32_t error);

/// What setLastHostError recorded last on this thread, 0 if nothing. Context::lastHostError()
/// returns it.
uint32_t lastHostError();

} // namespace deunify::detail
