BINDIR=${BINDIR:-${PREFIX:-/usr/local}/bin}

printf '%s\n' "Installing to $BINDIR" >&2

cp -f redo "$BINDIR"
for lnk in ifchange ifcreate infofor
do
	ln -sf redo "$BINDIR/redo-$lnk"
done
