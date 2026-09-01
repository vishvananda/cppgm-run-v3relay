#include "preproc_host.h"

#include <ctime>
#include <stdexcept>
#include <utility>

extern "C" long int syscall(long int n, ...) throw ();

bool PA5GetFileId(const std::string& path, PA5FileId& out_fileid)
{
	struct
	{
		unsigned long int dev;
		unsigned long int ino;
		long int unused[16];
	} data;
	const int result = static_cast<int>(syscall(/* stat */ 4, path.c_str(), &data));
	out_fileid = std::make_pair(data.dev, data.ino);
	return result == 0;
}

PreprocBuildInfo PreprocHostBuildInfo()
{
	const time_t now = time(0);
	const tm* local_time = localtime(&now);
	if (local_time == 0)
		throw std::runtime_error("unable to obtain build time");
	const char* snapshot = asctime(local_time);
	if (snapshot == 0)
		throw std::runtime_error("unable to obtain build time");
	const std::string stamp(snapshot);
	if (stamp.size() < 24)
		throw std::runtime_error("invalid build time");
	PreprocBuildInfo build_info;
	build_info.date = stamp.substr(4, 7) + stamp.substr(20, 4);
	build_info.time = stamp.substr(11, 8);
	build_info.author = "Vishvananda";
	return build_info;
}
