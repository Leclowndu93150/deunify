#include "codecs.hpp"

#include <zlib.h>

#include <limits>

namespace deunify::detail {

namespace {

bool isCustom(Codec codec) {
	return static_cast<uint32_t>(codec) >= kFirstCustomCodec;
}

Status decodeCustom(Codec codec, const CustomDecoder& custom, std::span<const uint8_t> in,
                    std::span<uint8_t> out) {
	if (!isCustom(codec) || custom.decode == nullptr) {
		return Status::NotSupported;
	}
	Status status = custom.decode(custom.context, codec, in.data(), in.size(), out.data(), out.size());
	return status == Status::Ok ? Status::Ok : Status::DecodeFailed;
}

Status applyStep(Codec codec, uint32_t outBytes, const CustomDecoder& custom,
                 std::vector<uint8_t>& bytes) {
	if (codec == Codec::None) {
		return bytes.size() == outBytes ? Status::Ok : Status::DecodeFailed;
	}
	std::vector<uint8_t> out(outBytes);
	Status status = codec == Codec::Zlib ? inflateZlib(bytes, out) : decodeCustom(codec, custom, bytes, out);
	if (status == Status::Ok) {
		bytes = std::move(out);
	}
	return status;
}

} // namespace

Status inflateZlib(std::span<const uint8_t> in, std::span<uint8_t> out) {
	constexpr uint64_t kLargest = std::numeric_limits<uLong>::max();
	if (in.size() > kLargest || out.size() > kLargest) {
		return Status::DecodeFailed;
	}
	uLongf produced = static_cast<uLongf>(out.size());
	int    result   = uncompress(out.data(), &produced, in.data(), static_cast<uLong>(in.size()));
	return result == Z_OK && produced == out.size() ? Status::Ok : Status::DecodeFailed;
}

Status decode(const CodecSteps& steps, const CustomDecoder& custom, std::vector<uint8_t>& bytes) {
	if (steps.second == Codec::None) {
		return applyStep(steps.first, steps.destinationBytes, custom, bytes);
	}
	Status status = applyStep(steps.first, steps.intermediateBytes, custom, bytes);
	if (status != Status::Ok) {
		return status;
	}
	return applyStep(steps.second, steps.destinationBytes, custom, bytes);
}

} // namespace deunify::detail
