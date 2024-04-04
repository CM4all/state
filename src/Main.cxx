// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "io/DirectoryReader.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "util/PrintException.hxx"
#include "util/SpanCast.hxx"
#include "util/StringAPI.hxx"
#include "util/StringStrip.hxx"

#include <fmt/core.h>

#include <map>
#include <string>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct StateDirectory {
	const char *name;
	const char *path;
};

static constexpr StateDirectory directories[] = {
	{"lib", "/lib/cm4all/state"},
	{"var", "/var/lib/cm4all/state"},
	{"etc", "/etc/cm4all/state"},
	{"run", "/run/cm4all/state"},
};

struct StateTreeNode {
	const char *source;

	std::map<std::string, StateTreeNode, std::less<>> children;

	std::string value;
};

static bool
SkipFilename(const char *name) noexcept
{
	return *name == 0 || *name == '.';
}

static void
LoadDirectory(const char *source, UniqueFileDescriptor _directory_fd, StateTreeNode &directory_node)
{
	DirectoryReader r{std::move(_directory_fd)};
	const FileDescriptor directory_fd = r.GetFileDescriptor();

	while (const char *name = r.Read()) {
		if (SkipFilename(name))
			continue;

		UniqueFileDescriptor fd;
		/* optimistic open() - this works for regular files
		   and directories */
		if (!fd.Open(directory_fd, name, O_RDONLY|O_NOFOLLOW)) {
			fmt::print(stderr, "Failed to open {:?}: {}\n",
				   name, strerror(errno));
			continue;
		}

		struct statx stx;
		if (statx(fd.Get(), "",
			  AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW|AT_STATX_SYNC_AS_STAT,
			  STATX_TYPE, &stx) < 0) {
			fmt::print(stderr, "Failed to stat {:?}: {}\n",
				   name, strerror(errno));
			continue;
		}

		auto [it, inserted] = directory_node.children.try_emplace(name);
		auto &child_node = it->second;
		child_node.source = source;

		if (S_ISDIR(stx.stx_mode)) {
			child_node.value.clear();

			LoadDirectory(source, std::move(fd), child_node);
		} else if (S_ISREG(stx.stx_mode)) {
			child_node.children.clear();
			child_node.value.clear();

			if (stx.stx_size > 0) {
				std::byte buffer[256];
				const auto nbytes = fd.Read(buffer);
				if (nbytes < 0) {
					fmt::print(stderr, "Failed to read {:?}: {}\n",
						   name, strerror(errno));
					continue;
				}

				child_node.value = Strip(ToStringView(std::span{buffer}.first(nbytes)));
			}
		}
	}
}

static void
Dump(std::string &path, const StateTreeNode &node) noexcept
{
	if (!node.value.empty()) {
		fmt::print("{} [{}] {:?}\n", path, node.source, node.value);
		return;
	}

	const std::size_t path_length = path.length();
	path.push_back('/');

	for (const auto &[name, child] : node.children) {
		path.erase(path_length + 1);
		path.append(name);

		Dump(path, child);
	}

	path.erase(path_length);
}

static void
Dump()
{
	StateTreeNode root;

	for (const auto &i : directories) {
		UniqueFileDescriptor fd;
		if (!fd.Open(i.path, O_DIRECTORY|O_RDONLY)) {
			fmt::print(stderr, "Failed to open {:?}: {}\n",
				   i.path, strerror(errno));
			continue;
		}

		LoadDirectory(i.name, std::move(fd), root);
	}

	std::string path;
	Dump(path, root);
}

int
main(int argc, char **argv)
try {
	if (argc < 2) {
		fmt::print(stderr, "Usage: {} COMMAND [OPTIONS]\n", argv[0]);
		return EXIT_FAILURE;
	}

	const char *const command = argv[1];
	if (StringIsEqual(command, "dump")) {
		Dump();
	} else {
		fmt::print(stderr, "Unknown command: {:?}\n", command);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
} catch (...) {
	PrintException(std::current_exception());
	return EXIT_FAILURE;
}
