// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "lib/fmt/SystemError.hxx"
#include "io/DirectoryReader.hxx"
#include "io/FileWriter.hxx"
#include "io/Open.hxx"
#include "io/StateDirectories.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "util/IterableSplitString.hxx"
#include "util/PrintException.hxx"
#include "util/SpanCast.hxx"
#include "util/StringAPI.hxx"
#include "util/StringSplit.hxx"
#include "util/StringStrip.hxx"

#include <fmt/core.h>

#include <map>
#include <string>
#include <string_view>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

using std::string_view_literals::operator""sv;

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
	StateTreeNode *const parent;

	const char *source;

	/**
	 * The target of the symlink.
	 */
	std::string target;

	std::map<std::string, StateTreeNode, std::less<>> children;

	std::string value;

	explicit StateTreeNode(StateTreeNode *_parent) noexcept
		:parent(_parent) {}

	[[gnu::pure]]
	const StateTreeNode *LookupPath(std::string_view path) const noexcept;
};

const StateTreeNode *
StateTreeNode::LookupPath(std::string_view path) const noexcept
{
	const StateTreeNode *node = this;

	if (path.starts_with('/')) {
		while (node->parent != nullptr)
			node = node->parent;

		path.remove_prefix(1);
	}

	for (const std::string_view segment : IterableSplitString(path, '/')) {
		if (segment.empty() || segment == "."sv)
			continue;

		if (segment == ".."sv) {
			node = node->parent;
			if (node == nullptr)
				return nullptr;
			continue;
		}

		auto i = node->children.find(segment);
		if (i == node->children.end())
			return nullptr;

		node = &i->second;
		if (!node->target.empty()) {
			// TODO loop detection
			node = node->LookupPath(node->target);
			if (node == nullptr)
				return nullptr;
		}
	}

	return node;
}

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
			if (errno == ELOOP) {
				char target[4096];
				if (auto nbytes = readlinkat(directory_fd.Get(), name,
							     target, sizeof(target));
				    nbytes < 0) {
					fmt::print(stderr, "Failed to read symlink {:?}: {}\n",
						   name, strerror(errno));
					continue;
				} else if (static_cast<std::size_t>(nbytes) == sizeof(target)) {
					fmt::print(stderr, "Symlink {:?} is too long\n",
						   name);
					continue;
				}

				auto [it, inserted] =
					directory_node.children.try_emplace(name,
									    &directory_node);
				auto &child_node = it->second;
				child_node.source = source;
				child_node.target = target;
				child_node.children.clear();
				child_node.value.clear();
				continue;
			}

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

		auto [it, inserted] =
			directory_node.children.try_emplace(name,
							    &directory_node);
		auto &child_node = it->second;
		child_node.source = source;

		if (S_ISDIR(stx.stx_mode)) {
			child_node.target.clear();
			child_node.value.clear();

			LoadDirectory(source, std::move(fd), child_node);
		} else if (S_ISREG(stx.stx_mode)) {
			child_node.target.clear();
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
	if (!node.target.empty()) {
		fmt::print("{} [{}] -> {}"sv, path, node.source, node.target);

		const auto *target = node.parent != nullptr
			? node.parent->LookupPath(node.target)
			: nullptr;
		if (target == nullptr) {
			fmt::print(" [not_found]"sv);
		} else if (!target->value.empty()) {
			fmt::print(" [{}] {:?}"sv, target->source, target->value);
		} else if (!target->children.empty()) {
			fmt::print(" [directory]"sv);
		} else {
			fmt::print(" [empty]"sv);
		}

		fmt::print("\n"sv);

		return;
	}

	if (!node.value.empty()) {
		fmt::print("{} [{}] {:?}\n", path, node.source, node.value);
		return;
	}

	if (!path.empty()) 
		path.push_back('/');

	const std::size_t path_length = path.length();

	for (const auto &[name, child] : node.children) {
		path.append(name);
		Dump(path, child);
		path.erase(path_length);
	}
}

static void
Dump(std::span<const char *const> args)
{
	if (!args.empty())
		throw "Too many parameters";

	StateTreeNode root{nullptr};

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

static void
Get(std::span<const char *const> args)
{
	if (args.empty())
		throw "Not enough parameters";

	if (args.size() > 1)
		throw "Too many parameters";

	const char *const path = args.front();
	if (*path == '\0' || *path == '/')
		throw "Bad path";

	StateDirectories state_directories;

	std::byte buffer[4096];

	const auto value = state_directories.GetBinary(path, buffer);
	if (value.data() == nullptr)
		throw "Not found";

	const auto text = Strip(ToStringView(value));
	fmt::print("{}\n", text);
}

static UniqueFileDescriptor
MakeSubdirectory(UniqueFileDescriptor parent_fd, const char *name)
{
	UniqueFileDescriptor fd;
	if (fd.Open(parent_fd, name, O_PATH|O_DIRECTORY|O_NOFOLLOW))
		return fd;

	int e = errno;
	if (e == ENOENT) {
		if (mkdirat(parent_fd.Get(), name, 0777) < 0) {
			e = errno;
			if (e != EEXIST)
				throw FmtErrno(e, "Failed to create directory {:?}", name);
		}

		if (fd.Open(parent_fd, name, O_PATH|O_DIRECTORY|O_NOFOLLOW))
			return fd;

		e = errno;
	} else if (e == ELOOP || e == ENOTDIR) {
		if (unlinkat(parent_fd.Get(), name, 0) < 0) {
			e = errno;
			if (e != ENOENT)
				throw FmtErrno(e, "Failed to delete symlink {:?}", name);
		}

		if (mkdirat(parent_fd.Get(), name, 0777) < 0) {
			e = errno;
			if (e != EEXIST)
				throw FmtErrno(e, "Failed to create directory {:?}", name);
		}

		if (fd.Open(parent_fd, name, O_PATH|O_DIRECTORY|O_NOFOLLOW))
			return fd;

		e = errno;
	}

	throw FmtErrno(e, "Failed to open directory {:?}", name);
}

static UniqueFileDescriptor
MakeSubdirectories(UniqueFileDescriptor fd, std::string_view relative_path)
{
	for (const std::string_view name : IterableSplitString(relative_path, '/'))
		fd = MakeSubdirectory(std::move(fd), std::string{name}.c_str());

	return fd;
}

static void
Set(std::span<const char *const> args)
{
	if (args.size() < 3)
		throw "Not enough parameters";

	if (args.size() > 3)
		throw "Too many parameters";

	const char *base_path = args[0];
	if (StringIsEqual(base_path, "--var"))
		base_path = "/var/lib/cm4all/state";
	else if (StringIsEqual(base_path, "--etc"))
		base_path = "/etc/cm4all/state";
	else if (StringIsEqual(base_path, "--run"))
		base_path = "/run/cm4all/state";
	else
		throw "No base directory specified";

	const char *const relative_path = args[1];
	const std::string_view value = args[2];

	if (*relative_path == '\0' || *relative_path == '/')
		throw "Bad path";

	const char *const slash = strrchr(relative_path, '/');
	const std::string_view directory_path{relative_path, slash};
	const char *const filename = slash + 1;

	if (*filename == '\0')
		throw "Bad path";

	auto directory_fd = MakeSubdirectories(OpenPath(FileDescriptor::Undefined(), base_path, O_DIRECTORY),
					       directory_path);
	FileWriter w{directory_fd, filename};
	w.Write(AsBytes(value));
	w.Commit();
}

int
main(int argc, char **argv)
try {
	if (argc < 2) {
		fmt::print(stderr, "Usage: {} COMMAND [OPTIONS]\n"
			   "\n"
			   "Commands:\n"
			   "  get PATH\n"
			   "  set {{--var|--etc|--run}} PATH VALUE\n"
			   "  dump\n"
			   "\n", argv[0]);
		return EXIT_FAILURE;
	}

	const char *const command = argv[1];
	std::span<const char *const> args{argv + 2, static_cast<std::size_t>(argc - 2)};

	if (StringIsEqual(command, "dump")) {
		Dump(args);
	} else if (StringIsEqual(command, "get")) {
		Get(args);
	} else if (StringIsEqual(command, "set")) {
		Set(args);
	} else {
		fmt::print(stderr, "Unknown command: {:?}\n", command);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
} catch (...) {
	PrintException(std::current_exception());
	return EXIT_FAILURE;
}
