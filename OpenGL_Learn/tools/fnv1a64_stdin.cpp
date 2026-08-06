#include <array>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main()
{
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
#endif
	std::uint64_t hash = 1469598103934665603ull;
	std::array<unsigned char, 64u * 1024u> buffer{};
	while (std::cin) {
		std::cin.read(
			reinterpret_cast<char*>(buffer.data()),
			static_cast<std::streamsize>(buffer.size()));
		const std::streamsize count = std::cin.gcount();
		for (std::streamsize index = 0; index < count; ++index) {
			hash ^= buffer[static_cast<std::size_t>(index)];
			hash *= 1099511628211ull;
		}
	}
	std::cout << "0x" << std::hex << std::setw(16) << std::setfill('0')
		<< hash << '\n';
	return std::cin.bad() ? 1 : 0;
}
