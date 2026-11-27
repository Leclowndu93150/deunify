#include "hostError.hpp"

namespace deunify::detail {

namespace {

thread_local uint32_t t_lastHostError = 0;

} // namespace

void setLastHostError(uint32_t error) {
	t_lastHostError = error;
}

uint32_t lastHostError() {
	return t_lastHostError;
}

} // namespace deunify::detail
