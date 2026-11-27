#pragma once

#include <windows.h>

#include <zlib.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace deunify::test {

class TempFile {
public:
	explicit TempFile(const std::vector<uint8_t>& bytes) {
		wchar_t directory[MAX_PATH] = {};
		wchar_t name[MAX_PATH]      = {};
		GetTempPathW(MAX_PATH, directory);
		GetTempFileNameW(directory, L"dfy", 0, name);
		m_path = name;
		std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}

	~TempFile() { DeleteFileW(m_path.c_str()); }

	TempFile(const TempFile&)            = delete;
	TempFile& operator=(const TempFile&) = delete;

	const wchar_t* path() const { return m_path.c_str(); }

private:
	std::wstring m_path;
};

inline std::vector<uint8_t> pattern(size_t bytes, uint32_t seed) {
	std::vector<uint8_t> out(bytes);
	for (size_t i = 0; i < bytes; ++i) {
		out[i] = static_cast<uint8_t>(i * 13 + i / 509 + seed * 31);
	}
	return out;
}

inline std::vector<uint8_t> compressed(const std::vector<uint8_t>& plain) {
	uLongf               bytes = compressBound(static_cast<uLong>(plain.size()));
	std::vector<uint8_t> out(bytes);
	compress2(out.data(), &bytes, plain.data(), static_cast<uLong>(plain.size()), Z_BEST_SPEED);
	out.resize(bytes);
	return out;
}

} // namespace deunify::test
