BINDIR=${BINDIR:-${PREFIX:-/usr/local}/bin}

cp -f redo "$BINDIR"
for lnk in ifchange ifcreate infofor
do
	ln -sf redo "$BINDIR/redo-$lnk"
done
