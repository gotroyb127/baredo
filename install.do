PREFIX=${PREFIX:-/usr/local}

redo all
cp -f redo redo-ifchange redo-ifcreate "$PREFIX/bin"
