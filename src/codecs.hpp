#pragma once

#include "deunify/context.hpp"
#include "deunify/types.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace deunify::detail {

/// The emulator's decoder for codecs from kFirstCustomCodec on (Callbacks::decode).
struct CustomDecoder {
	DecodeFn decode  = nullptr;
	void*    context = nullptr;
};

/// The codecs of one request and the sizes they produce (LoadRequest's fields).
struct CodecSteps {
	Codec    first             = Codec::None;
	Codec    second            = Codec::None;
	uint32_t intermediateBytes = 0;
	uint32_t destinationBytes  = 0;
};

/// Inflates a zlib stream (RFC 1950) into exactly `out.size()` bytes. Status::DecodeFailed if the
/// stream is damaged, incomplete or does not produce exactly that many bytes.
Status inflateZlib(std::span<const uint8_t> in, std::span<uint8_t> out);

/// Applies `steps` to `bytes` (the file bytes), which become the decoded result. Codec::None
/// requires its input to have the size it is to produce. A custom codec without a decoder, or an
/// unknown built-in codec, is Status::NotSupported.
Status decode(const CodecSteps& steps, const CustomDecoder& custom, std::vector<uint8_t>& bytes);

} // namespace deunify::detail
