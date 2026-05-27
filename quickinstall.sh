#!/usr/bin/env bash

set -euo pipefail

readonly program="gd-frequency"

build_dir="builddir"
mode="install"
system_install=false
custom_prefix=false
create_command_link=true
native_optimizations=true
block_size=64
cxx=${CXX:-}

user_prefix="${GD_FREQUENCY_PREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/gd-frequency}"
system_prefix="/opt/gd-frequency"
prefix="$user_prefix"

user_link_dir="${GD_FREQUENCY_LINK_DIR:-$HOME/.local/bin}"
system_link_dir="/usr/local/bin"
link_dir="$user_link_dir"

usage() {
   cat <<'EOF'
usage: ./quickinstall.sh [OPTIONS]

Installs gd-frequency into one self-contained prefix and optionally creates a
small command symlink on PATH. The default is a no-sudo user install:

   ~/.local/share/gd-frequency/      binary, dictionaries, cache
   ~/.local/bin/gd-frequency         symlink

OPTIONS
   --user                  install to the default user prefix (default)
   --system                install to /opt/gd-frequency and link from /usr/local/bin
   --prefix PATH           install to a custom self-contained prefix
   --link-dir PATH         directory where the gd-frequency command symlink is created
   --no-link               do not create a command symlink
   --uninstall             remove the selected install prefix and managed symlink
   --build-dir PATH        Meson build directory (default: builddir)
   --cxx COMMAND           C++ compiler command (default: clang++, then c++, then g++)
   --no-native             disable -march=native/-mtune=native
   --block-size N          compressed cache lookup block size (default: 64)
   --verbose               trace shell commands
   -h, --help              show this help

EXAMPLES
   ./quickinstall.sh
   ./quickinstall.sh --uninstall
   ./quickinstall.sh --system
   ./quickinstall.sh --prefix "$HOME/opt/gd-frequency" --link-dir "$HOME/.local/bin"
EOF
}

die() {
	echo "error: $*" >&2
	exit 1
}

need_value() {
	local option=$1
	local value=${2:-}
	[[ -n "$value" ]] || die "$option requires a value"
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--user)
			system_install=false
			if ! $custom_prefix; then
				prefix="$user_prefix"
			fi
			link_dir="$user_link_dir"
			shift
			;;
		--system)
			system_install=true
			if ! $custom_prefix; then
				prefix="$system_prefix"
			fi
			link_dir="$system_link_dir"
			shift
			;;
		--prefix)
			need_value "$1" "${2:-}"
			prefix=$2
			custom_prefix=true
			shift 2
			;;
		--link-dir)
			need_value "$1" "${2:-}"
			link_dir=$2
			shift 2
			;;
		--no-link)
			create_command_link=false
			shift
			;;
		--uninstall)
			mode="uninstall"
			shift
			;;
		--build-dir)
			need_value "$1" "${2:-}"
			build_dir=$2
			shift 2
			;;
		--cxx)
			need_value "$1" "${2:-}"
			cxx=$2
			shift 2
			;;
		--no-native)
			native_optimizations=false
			shift
			;;
		--block-size)
			need_value "$1" "${2:-}"
			block_size=$2
			shift 2
			;;
		--verbose)
			set -x
			shift
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			die "unknown option: $1"
			;;
	esac
done

prefix=${prefix%/}
link_dir=${link_dir%/}
sentinel="$prefix/.gd-frequency-install"
command_link="$link_dir/$program"
command_target="$prefix/bin/$program"

run_privileged() {
	if $system_install && [[ ${EUID:-$(id -u)} -ne 0 ]]; then
		command -v sudo >/dev/null 2>&1 || die "sudo is required for --system"
		sudo "$@"
	else
		"$@"
	fi
}

check_command() {
	command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

select_compiler() {
	if [[ -n "$cxx" ]]; then
		command -v "$cxx" >/dev/null 2>&1 || die "compiler not found: $cxx"
		return
	fi

	for candidate in clang++ c++ g++; do
		if command -v "$candidate" >/dev/null 2>&1; then
			cxx=$candidate
			return
		fi
	done

	die "no C++ compiler found; install clang++, c++, or g++"
}

assert_safe_prefix() {
	[[ -n "$prefix" ]] || die "empty install prefix"

	case "$prefix" in
		"/"|"/usr"|"/usr/local"|"/opt"|"$HOME"|"$HOME/.local"|"$HOME/.local/share")
			die "refusing broad prefix: $prefix; choose a dedicated gd-frequency directory"
			;;
	esac
}

prefix_looks_managed() {
	[[ -f "$sentinel" ]] && return 0
	[[ -x "$prefix/bin/$program" && -d "$prefix/share/gd-frequency" ]] && return 0
	[[ -d "$prefix" && -z $(find "$prefix" -mindepth 1 -maxdepth 1 -print -quit) ]] && return 0
	[[ ! -e "$prefix" ]] && return 0
	return 1
}

remove_command_link() {
	if [[ -L "$command_link" ]]; then
		local target
		target=$(readlink "$command_link")
		if [[ "$target" == "$command_target" || "$target" == "$prefix/"* ]]; then
			run_privileged rm -f "$command_link"
		else
			echo "leaving unrelated symlink: $command_link -> $target" >&2
		fi
	elif [[ -e "$command_link" ]]; then
		echo "leaving unrelated file: $command_link" >&2
	fi
}

remove_prefix() {
	assert_safe_prefix
	if prefix_looks_managed; then
		run_privileged rm -rf "$prefix"
	else
		die "refusing to remove unmanaged prefix: $prefix"
	fi
}

create_link() {
	$create_command_link || return 0

	run_privileged install -d "$link_dir"

	if [[ -e "$command_link" && ! -L "$command_link" ]]; then
		die "cannot create link because a non-symlink already exists: $command_link"
	fi

	remove_command_link
	run_privileged ln -s "$command_target" "$command_link"

	case ":$PATH:" in
		*":$link_dir:"*) ;;
		*) echo "note: $link_dir is not currently on PATH" >&2 ;;
	esac
}

write_sentinel() {
	local tmp
	tmp=$(mktemp)
	{
		printf 'program=%s\n' "$program"
		printf 'prefix=%s\n' "$prefix"
		printf 'link=%s\n' "$command_link"
		printf 'compiler=%s\n' "$cxx"
		printf 'block_size=%s\n' "$block_size"
	} > "$tmp"

	run_privileged install -m 0644 "$tmp" "$sentinel"
	rm -f "$tmp"
}

install_program() {
	check_command meson
	check_command ninja
	select_compiler
	assert_safe_prefix

	if [[ -e "$prefix" ]] && ! prefix_looks_managed; then
		die "prefix exists but does not look managed by gd-frequency: $prefix"
	fi

	remove_command_link
	remove_prefix

	CXX=$cxx meson setup "$build_dir" \
		--wipe \
		--prefix "$prefix" \
		-Dnative_optimizations="$native_optimizations" \
		-Dblock_size="$block_size"

	meson compile -C "$build_dir"
	run_privileged meson install -C "$build_dir"

	write_sentinel
	create_link

	echo "installed $program to $prefix"
	if $create_command_link; then
		echo "command link: $command_link -> $command_target"
	fi
}

uninstall_program() {
	assert_safe_prefix
	remove_command_link
	remove_prefix
	echo "uninstalled $program from $prefix"
}

case "$mode" in
	install)
		install_program
		;;
	uninstall)
		uninstall_program
		;;
	*)
		die "unknown mode: $mode"
		;;
esac
