grep "^$1\$" lnks > /dev/null || {
	printf '%s\n' "wrong target: $1" >&2
	exit 1
}

ln -sf redo $3
